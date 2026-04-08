// ============================================================================
// OVERLORD — admin dashboard JS.
//
// Single-file SPA — no framework, no bundler. Talks to the relay's
// /overlord/api/* endpoints (built in Phase C). Token lives in
// localStorage.overlord_token (separate from the spectator app's
// nobd_token so /overlord and / can have different auth state).
//
// On any 401/403 from any endpoint, kicks the user back to the login
// page so the dashboard never gets stuck in a "stale auth, nothing
// updates" state.
// ============================================================================

const API = '/overlord/api';
const TOKEN_KEY = 'overlord_token';
const USERNAME_KEY = 'overlord_username';
const POLL_INTERVAL_MS = 2000;

// ----------------------------------------------------------------------------
// Auth + fetch wrapper
// ----------------------------------------------------------------------------

function getToken() {
  return localStorage.getItem(TOKEN_KEY) || '';
}

function logoutAndRedirect() {
  localStorage.removeItem(TOKEN_KEY);
  localStorage.removeItem(USERNAME_KEY);
  window.location.href = '/overlord/login.html';
}

window.overlordLogout = logoutAndRedirect;

async function api(path, opts = {}) {
  const headers = {
    'Authorization': `Bearer ${getToken()}`,
    ...(opts.headers || {}),
  };
  if (opts.body && !(opts.body instanceof FormData) && !headers['Content-Type']) {
    headers['Content-Type'] = 'application/json';
  }
  const res = await fetch(API + path, { ...opts, headers });
  if (res.status === 401 || res.status === 403) {
    toast('error', 'AUTH', 'Session expired. Redirecting to login.');
    setTimeout(logoutAndRedirect, 800);
    throw new Error('auth');
  }
  return res;
}

async function apiJson(path, opts = {}) {
  const res = await api(path, opts);
  return res.json().catch(() => ({ ok: false, error: 'invalid response' }));
}

// ----------------------------------------------------------------------------
// Toasts (transient notifications, top of card actions / bottom-right)
// ----------------------------------------------------------------------------

function toast(kind, title, body, duration = 3500) {
  const container = document.getElementById('toastContainer');
  const el = document.createElement('div');
  el.className = `toast toast-${kind}`;
  el.innerHTML = `<div class="toast-title">${escapeHtml(title)}</div><div class="toast-body">${escapeHtml(body)}</div>`;
  container.appendChild(el);
  setTimeout(() => {
    el.style.transition = 'opacity 0.3s, transform 0.3s';
    el.style.opacity = '0';
    el.style.transform = 'translateX(20px)';
    setTimeout(() => el.remove(), 300);
  }, duration);
}

function escapeHtml(s) {
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#039;');
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

function fmtBytes(n) {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MB`;
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GB`;
}

function fmtUptime(s) {
  if (s == null) return '—';
  if (s < 60) return `${s}s`;
  if (s < 3600) return `${Math.floor(s / 60)}m ${s % 60}s`;
  if (s < 86400) {
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    return `${h}h ${m}m`;
  }
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  return `${d}d ${h}h`;
}

function fmtMtime(unix) {
  if (!unix) return '—';
  const d = new Date(unix * 1000);
  // YYYY-MM-DD HH:MM
  const pad = n => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

// ----------------------------------------------------------------------------
// STATUS card
// ----------------------------------------------------------------------------

let statusPollHandle = null;
let lastStatus = null;

async function fetchStatus() {
  try {
    const data = await apiJson('/status');
    if (!data.ok) {
      setStatusBad(data.error || 'unknown');
      return;
    }
    renderStatus(data.data);
    lastStatus = data.data;
  } catch (e) {
    if (e.message !== 'auth') setStatusBad(e.message || 'connection error');
  }
}

// Rolling history buffers for the metric tiles' sparklines.
const SPARK_LEN = 30;  // ~60 seconds at 2s poll interval
const sparkData = {
  fps: [],
  mbps: [],
  clients: [],
};

function pushSpark(key, val) {
  sparkData[key].push(val);
  while (sparkData[key].length > SPARK_LEN) sparkData[key].shift();
}

function renderSpark(elId, key, color) {
  const el = document.getElementById(elId);
  if (!el) return;
  const data = sparkData[key];
  if (data.length < 2) { el.innerHTML = ''; return; }
  const w = 100, h = 100;
  const min = Math.min(...data, 0);
  const max = Math.max(...data, 1);
  const range = (max - min) || 1;
  const step = w / (SPARK_LEN - 1);
  // Right-align: most recent on the right, oldest on the left.
  const offset = (SPARK_LEN - data.length) * step;
  const points = data.map((v, i) => {
    const x = offset + i * step;
    const y = h - ((v - min) / range) * h;
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  });
  // Filled area under the line for visual weight.
  const areaPath = `M ${points[0]} L ${points.join(' L ')} L ${w},${h} L ${offset},${h} Z`;
  const linePath = `M ${points.join(' L ')}`;
  el.innerHTML = `<svg viewBox="0 0 ${w} ${h}" preserveAspectRatio="none">
    <path d="${areaPath}" fill="${color}" fill-opacity="0.15" stroke="none"/>
    <path d="${linePath}" stroke="${color}" stroke-linejoin="round" stroke-linecap="round"/>
  </svg>`;
}

function renderStatus(s) {
  const pillEl = document.getElementById('topStatusPill');
  const pillText = document.getElementById('topStatusText');
  const dot = pillEl.querySelector('.dot');
  if (s.flycast_active && s.upstream_connected !== false) {
    pillText.textContent = 'RUNNING';
    dot.className = 'dot dot-green';
  } else if (s.flycast_active) {
    pillText.textContent = 'NO UPSTREAM';
    dot.className = 'dot dot-amber';
  } else {
    pillText.textContent = 'STOPPED';
    dot.className = 'dot dot-red';
  }

  // ---- Metric tiles ----
  const fpsEl = document.getElementById('metricFps');
  const mbpsEl = document.getElementById('metricMbps');
  const clientsEl = document.getElementById('metricClients');

  if (s.fps != null) {
    fpsEl.textContent = s.fps.toFixed(1);
    fpsEl.className = 'metric-val ' + (s.fps >= 58 ? 'metric-good' : s.fps >= 50 ? 'metric-warn' : 'metric-bad');
    pushSpark('fps', s.fps);
    renderSpark('sparkFps', 'fps', s.fps >= 58 ? '#00d65a' : s.fps >= 50 ? '#ffa500' : '#ff3344');
  }
  if (s.mbps != null) {
    mbpsEl.textContent = s.mbps.toFixed(1);
    pushSpark('mbps', s.mbps);
    renderSpark('sparkMbps', 'mbps', '#5599ff');
  }
  if (s.clients != null) {
    clientsEl.textContent = String(s.clients);
    pushSpark('clients', s.clients);
    renderSpark('sparkClients', 'clients', '#ffd700');
  }

  // ---- Status grid ----
  document.getElementById('statusService').textContent = s.flycast_active ? 'active (running)' : 'stopped';
  document.getElementById('statusService').className = `status-val ${s.flycast_active ? 'good' : 'bad'}`;
  document.getElementById('statusPid').textContent = s.flycast_pid != null ? String(s.flycast_pid) : '—';
  document.getElementById('statusUptime').textContent = fmtUptime(s.flycast_uptime_s);
  document.getElementById('statusMem').textContent = s.flycast_mem_mb != null ? `${s.flycast_mem_mb} MB` : '—';
  document.getElementById('statusSlot').textContent = s.current_slot != null ? `slot ${s.current_slot}` : '—';
  document.getElementById('statusControlWs').textContent = s.control_ws || '—';

  const jitterEl = document.getElementById('statusJitter');
  if (jitterEl) {
    if (s.frame_jitter_ms != null) {
      jitterEl.textContent = `${s.frame_jitter_ms.toFixed(1)} ms`;
      jitterEl.className = 'status-val ' + (s.frame_jitter_ms < 25 ? 'good' : s.frame_jitter_ms < 50 ? '' : 'bad');
    } else {
      jitterEl.textContent = '—';
    }
  }
  const framesEl = document.getElementById('statusFrames');
  if (framesEl) {
    framesEl.textContent = s.frames_received != null ? s.frames_received.toLocaleString() : '—';
  }

  const meta = `${s.rom_basename || '—'} @ ${s.savestate_dir || '—'}`;
  document.getElementById('statusMeta').textContent = meta;
}

function setStatusBad(reason) {
  const pillEl = document.getElementById('topStatusPill');
  const pillText = document.getElementById('topStatusText');
  const dot = pillEl.querySelector('.dot');
  pillText.textContent = 'ERROR';
  dot.className = 'dot dot-red';
  document.getElementById('statusMeta').textContent = reason;
}

function startStatusPoll() {
  fetchStatus();
  if (statusPollHandle) clearInterval(statusPollHandle);
  statusPollHandle = setInterval(fetchStatus, POLL_INTERVAL_MS);
}

// ----------------------------------------------------------------------------
// SAVESTATES card
// ----------------------------------------------------------------------------

async function refreshSavestates() {
  try {
    const data = await apiJson('/savestates');
    if (!data.ok) {
      toast('error', 'SAVESTATES', data.error || 'failed to list');
      return;
    }
    renderSavestateRows(data.data);
  } catch (e) {
    if (e.message !== 'auth') toast('error', 'SAVESTATES', e.message);
  }
}

function renderSavestateRows(d) {
  const tbody = document.getElementById('savestateRows');
  const slots = d.slots || [];
  const meta = `${slots.length} slot${slots.length === 1 ? '' : 's'} in ${d.savestate_dir}`;
  document.getElementById('savestateMeta').textContent = meta;

  if (slots.length === 0) {
    tbody.innerHTML = '<tr><td colspan="5" class="muted">no savestates yet — use SAVE CURRENT &raquo; SLOT below</td></tr>';
    return;
  }

  tbody.innerHTML = slots.map(s => {
    const isCurrent = s.is_current ? ' row-current' : '';
    const currentBadge = s.is_current ? ' &laquo;LIVE' : '';
    const pinTitle = s.is_current
      ? 'Already the auto-load slot'
      : 'Set this slot as the auto-load slot for next boot. Requires RESTART SERVICE to take effect.';
    return `<tr class="row${isCurrent}">
      <td class="slot-num">${s.slot}${currentBadge}</td>
      <td>${escapeHtml(s.filename)}</td>
      <td>${fmtBytes(s.size)}</td>
      <td>${fmtMtime(s.mtime)}</td>
      <td class="row-actions">
        <button class="btn-row btn-pin" ${s.is_current ? 'disabled' : ''} onclick="pinSlot(${s.slot})" title="${pinTitle}">${s.is_current ? '★ AUTO' : '☆ PIN'}</button>
        <button class="btn-row btn-row-disabled" title="Hot-load disabled — see Phase A.2 known issue. Use PIN + RESTART instead.">LOAD</button>
        <button class="btn-row" onclick="downloadSlot(${s.slot})" title="Download this slot's .state file">DOWNLOAD</button>
      </td>
    </tr>`;
  }).join('');
}

window.pinSlot = async function(slot) {
  toast('info', 'PIN', `setting slot ${slot} as auto-load…`);
  try {
    const data = await apiJson('/savestates/set-autoload', {
      method: 'POST',
      body: JSON.stringify({ slot }),
    });
    if (!data.ok) {
      toast('error', 'PIN', data.error || 'failed');
      return;
    }
    toast('success', 'PIN', `slot ${slot} pinned. Click RESTART to apply.`);
    // Refresh the table to show the new pin star
    setTimeout(refreshSavestates, 200);
    // Also reload the config textarea so it shows the change
    setTimeout(reloadConfig, 200);
  } catch (e) {
    if (e.message !== 'auth') toast('error', 'PIN', e.message);
  }
};

window.downloadSlot = async function(slot) {
  try {
    const url = `${API}/savestates/download/${slot}`;
    const res = await api(`/savestates/download/${slot}`);
    if (!res.ok) {
      toast('error', 'DOWNLOAD', `slot ${slot} failed`);
      return;
    }
    const blob = await res.blob();
    const objUrl = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = objUrl;
    a.download = `mvc2_slot${slot}.state`;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(objUrl), 1000);
    toast('success', 'DOWNLOAD', `slot ${slot} (${fmtBytes(blob.size)})`);
  } catch (e) {
    if (e.message !== 'auth') toast('error', 'DOWNLOAD', e.message);
  }
};

window.saveCurrentTo = async function(slot) {
  if (!Number.isInteger(slot) || slot < 0 || slot > 99) {
    toast('error', 'SAVE', 'slot must be an integer 0-99');
    return;
  }
  toast('info', 'SAVE', `saving slot ${slot}…`);
  try {
    const data = await apiJson('/savestates/save', {
      method: 'POST',
      body: JSON.stringify({ slot }),
    });
    if (!data.ok) {
      toast('error', 'SAVE', data.error || 'failed');
      return;
    }
    toast('success', 'SAVE', `slot ${slot} written`);
    // Auto-refresh the table after a small delay so the file write
    // has time to land on disk.
    setTimeout(refreshSavestates, 400);
  } catch (e) {
    if (e.message !== 'auth') toast('error', 'SAVE', e.message);
  }
};

window.handleUpload = async function(input) {
  const file = input.files[0];
  if (!file) return;
  // Default to slot 9 — caller can rename later via the cfg editor.
  // We could prompt for a slot here but that's annoying UX. Just upload
  // to whatever the SAVE field has set, default 9.
  const slotInput = document.getElementById('newSlotNum');
  const slot = parseInt(slotInput.value, 10);
  if (!Number.isInteger(slot) || slot < 0 || slot > 99) {
    toast('error', 'UPLOAD', 'set slot field to 0-99 first');
    input.value = '';
    return;
  }
  toast('info', 'UPLOAD', `uploading ${file.name} → slot ${slot}…`);

  try {
    const fd = new FormData();
    fd.append('state', file, file.name);
    const res = await api(`/savestates/upload?slot=${slot}`, {
      method: 'POST',
      body: fd,
    });
    const data = await res.json().catch(() => ({ ok: false, error: 'invalid response' }));
    if (!data.ok) {
      toast('error', 'UPLOAD', data.error || 'failed');
      return;
    }
    toast('success', 'UPLOAD', `${data.data?.filename} (${fmtBytes(data.data?.size || 0)})`);
    setTimeout(refreshSavestates, 400);
  } catch (e) {
    if (e.message !== 'auth') toast('error', 'UPLOAD', e.message);
  } finally {
    input.value = '';
  }
};

window.refreshSavestates = refreshSavestates;

// ----------------------------------------------------------------------------
// CONFIG editor
// ----------------------------------------------------------------------------

let configOriginal = '';

async function reloadConfig() {
  try {
    const data = await apiJson('/config');
    if (!data.ok) {
      toast('error', 'CONFIG', data.error || 'load failed');
      return;
    }
    const text = data.data?.text || '';
    document.getElementById('configText').value = text;
    document.getElementById('configMeta').textContent = data.data?.path || '—';
    configOriginal = text;
    updateConfigDirty();
    document.getElementById('configBanner').style.display = 'none';
  } catch (e) {
    if (e.message !== 'auth') toast('error', 'CONFIG', e.message);
  }
}

function updateConfigDirty() {
  const cur = document.getElementById('configText').value;
  const indicator = document.getElementById('configDirty');
  indicator.textContent = (cur === configOriginal) ? '' : '* UNSAVED CHANGES';
}

async function saveConfig() {
  const text = document.getElementById('configText').value;
  if (text === configOriginal) {
    toast('info', 'CONFIG', 'nothing changed');
    return;
  }
  try {
    const data = await apiJson('/config', {
      method: 'POST',
      body: JSON.stringify({ text }),
    });
    if (!data.ok) {
      toast('error', 'CONFIG', data.error || 'save failed');
      return;
    }
    toast('success', 'CONFIG', `wrote ${data.data?.bytes || 0} bytes`);
    configOriginal = text;
    updateConfigDirty();
    document.getElementById('configBanner').style.display = '';
  } catch (e) {
    if (e.message !== 'auth') toast('error', 'CONFIG', e.message);
  }
}

window.reloadConfig = reloadConfig;
window.saveConfig = saveConfig;

// ----------------------------------------------------------------------------
// LOGS card
// ----------------------------------------------------------------------------

async function reloadLogs() {
  const n = parseInt(document.getElementById('logLines').value, 10) || 200;
  try {
    const data = await apiJson(`/logs/tail?n=${n}`);
    if (!data.ok) {
      document.getElementById('logViewer').textContent = `error: ${data.error}`;
      return;
    }
    renderLogs(data.data?.text || '');
  } catch (e) {
    if (e.message !== 'auth') {
      document.getElementById('logViewer').textContent = `error: ${e.message}`;
    }
  }
}

function renderLogs(text) {
  const viewer = document.getElementById('logViewer');
  // Color-code lines by content. Each line becomes a span.
  const lines = text.split('\n').map(line => {
    if (!line) return '<span class="log-line"> </span>';
    let cls = 'log-line';
    if (/E\[|error|fail/i.test(line)) cls += ' log-line-err';
    else if (/W\[|warn/i.test(line)) cls += ' log-line-warn';
    else if (/MIRROR/.test(line)) cls += ' log-line-mirror';
    else if (/control-ws/.test(line)) cls += ' log-line-control';
    else cls += ' log-line-info';
    return `<span class="${cls}">${escapeHtml(line)}</span>`;
  });
  viewer.innerHTML = lines.join('\n');
  // Auto-scroll to bottom
  viewer.scrollTop = viewer.scrollHeight;
}

window.reloadLogs = reloadLogs;

// ----------------------------------------------------------------------------
// SERVICE actions
// ----------------------------------------------------------------------------

async function restartService() {
  if (!confirm('Restart maplecast-headless? Spectators will see a 1-2 second glitch as the relay reconnects.')) return;
  toast('warn', 'RESTART', 'restarting service…');
  try {
    const data = await apiJson('/service/restart', { method: 'POST' });
    if (!data.ok) {
      toast('error', 'RESTART', data.error || 'failed');
      return;
    }
    toast('success', 'RESTART', 'service restarted');
    // Force-refresh status after restart so the pill flips quickly
    setTimeout(fetchStatus, 1000);
    setTimeout(fetchStatus, 3000);
    // Also reload the live preview iframe so it reconnects to the
    // new flycast process.
    setTimeout(() => {
      const iframe = document.getElementById('previewFrame');
      if (iframe) iframe.src = iframe.src;
    }, 2000);
  } catch (e) {
    if (e.message !== 'auth') toast('error', 'RESTART', e.message);
  }
}

window.restartService = restartService;

// ----------------------------------------------------------------------------
// PLAYERS card — slots + queue + kick/promote/backdoor
// ----------------------------------------------------------------------------

let playersPollHandle = null;

async function refreshPlayers() {
  try {
    const data = await apiJson('/players');
    if (!data.ok) {
      toast('error', 'PLAYERS', data.error || 'failed');
      return;
    }
    renderPlayers(data.data);
  } catch (e) {
    if (e.message !== 'auth') toast('error', 'PLAYERS', e.message);
  }
}

function fmtAge(unixIso) {
  if (!unixIso) return '—';
  const t = new Date(unixIso).getTime();
  if (!t) return '—';
  const ageS = Math.floor((Date.now() - t) / 1000);
  if (ageS < 0) return 'now';
  if (ageS < 60) return `${ageS}s ago`;
  if (ageS < 3600) return `${Math.floor(ageS / 60)}m ago`;
  if (ageS < 86400) return `${Math.floor(ageS / 3600)}h ago`;
  return `${Math.floor(ageS / 86400)}d ago`;
}

function renderPlayers(d) {
  const slots = d.slots || [];
  const queue = d.queue || [];

  // Slot tiles
  for (let i = 0; i < 2; i++) {
    const slot = slots.find(s => s.slot_num === i) || {};
    const tile = document.getElementById(`slotTile${i}`);
    const statusEl = document.getElementById(`slotTileStatus${i}`);
    const nameEl = document.getElementById(`slotTileName${i}`);
    const metaEl = document.getElementById(`slotTileMeta${i}`);
    const kickBtn = document.getElementById(`slotKick${i}`);

    if (slot.occupant_name) {
      tile.classList.add('occupied');
      statusEl.textContent = 'OCCUPIED';
      nameEl.textContent = String(slot.occupant_name).toUpperCase();
      const lastInput = slot.last_input_at ? `last input ${fmtAge(slot.last_input_at)}` : '';
      const claimedAt = slot.claimed_at ? `claimed ${fmtAge(slot.claimed_at)}` : '';
      const device = slot.device || '';
      metaEl.textContent = [device, claimedAt, lastInput].filter(Boolean).join(' · ') || 'no metadata';
      kickBtn.disabled = false;
      kickBtn.classList.remove('btn-row-disabled');
      kickBtn.classList.add('btn-row');
      kickBtn.classList.add('btn-pin');
      kickBtn.style.borderColor = 'var(--ov-red)';
      kickBtn.style.color = 'var(--ov-red)';
    } else {
      tile.classList.remove('occupied');
      statusEl.textContent = 'EMPTY';
      nameEl.textContent = '—';
      metaEl.textContent = 'no occupant';
      kickBtn.disabled = true;
      kickBtn.classList.add('btn-row-disabled');
      kickBtn.style.borderColor = '';
      kickBtn.style.color = '';
    }
  }

  // Queue list
  document.getElementById('queueMeta').textContent =
    queue.length === 0 ? 'empty' : `${queue.length} in line`;
  const list = document.getElementById('queueList');
  if (queue.length === 0) {
    list.innerHTML = '<div class="muted">no one in line</div>';
    return;
  }
  list.innerHTML = queue.map((q, i) => {
    const id = String(q.id || '');
    const escId = id.replace(/'/g, "\\'");
    const name = String(q.username || '?').toUpperCase();
    const isAnon = !!q.is_anon;
    const ago = fmtAge(q.joined_at);
    const device = q.device || '';
    return `<div class="queue-row ${isAnon ? 'qrow-anon' : ''}">
      <div class="qpos">${i + 1}</div>
      <div class="qname">${escapeHtml(name)}${isAnon ? ' <span style="font-size:9px;color:var(--ov-text-mute);">(anon)</span>' : ''}</div>
      <div class="qtime" title="${escapeHtml(device)}">${ago}</div>
      <div class="qactions">
        <button class="btn-row btn-promote" onclick="promoteQueue('${escId}', 0)" title="Promote to P1">→ P1</button>
        <button class="btn-row btn-promote" onclick="promoteQueue('${escId}', 1)" title="Promote to P2">→ P2</button>
        <button class="btn-row" style="border-color:var(--ov-red);color:var(--ov-red);" onclick="kickQueue('${escId}', '${escapeHtml(name)}')" title="Remove from queue">KICK</button>
      </div>
    </div>`;
  }).join('');
}

window.refreshPlayers = refreshPlayers;

window.kickSlot = async function(slot) {
  const occupantEl = document.getElementById(`slotTileName${slot}`);
  const occupant = (occupantEl?.textContent || '').trim();
  if (!occupant || occupant === '—') return;
  if (!confirm(`Kick ${occupant} from P${slot + 1}?`)) return;

  toast('warn', 'KICK', `kicking ${occupant} from P${slot + 1}…`);
  try {
    const data = await apiJson('/players/kick', {
      method: 'POST',
      body: JSON.stringify({ slot }),
    });
    if (!data.ok) {
      toast('error', 'KICK', data.error || 'failed');
      return;
    }
    const kicked = data.data?.kicked || occupant;
    toast('success', 'KICK', `${kicked.toUpperCase()} cleared from P${slot + 1}`);
    setTimeout(refreshPlayers, 200);
  } catch (e) {
    if (e.message !== 'auth') toast('error', 'KICK', e.message);
  }
};

window.kickQueue = async function(queueId, displayName) {
  if (!confirm(`Remove ${displayName} from the queue?`)) return;
  try {
    const data = await apiJson('/queue/kick', {
      method: 'POST',
      body: JSON.stringify({ queue_id: queueId }),
    });
    if (!data.ok) {
      toast('error', 'QKICK', data.error || 'failed');
      return;
    }
    toast('success', 'QKICK', `${displayName} removed from queue`);
    setTimeout(refreshPlayers, 200);
  } catch (e) {
    if (e.message !== 'auth') toast('error', 'QKICK', e.message);
  }
};

window.promoteQueue = async function(queueId, slot) {
  try {
    const data = await apiJson('/queue/promote', {
      method: 'POST',
      body: JSON.stringify({ queue_id: queueId, slot }),
    });
    if (!data.ok) {
      toast('error', 'PROMOTE', data.error || 'failed');
      return;
    }
    toast('success', 'PROMOTE', `promoted → P${slot + 1}`);
    setTimeout(refreshPlayers, 400);
  } catch (e) {
    if (e.message !== 'auth') toast('error', 'PROMOTE', e.message);
  }
};

// ----------------------------------------------------------------------------
// BACKDOOR PLAY — open king.html in a new tab as the admin user
// ----------------------------------------------------------------------------
//
// The admin user is already registered as a normal player record (we
// flagged its admin bit in Phase B). Their nobd_token / nobd_username
// might or might not be set in localStorage depending on whether they
// also use the spectator app. We seed both keys before opening king.html
// so the spectator app autosigns in, then optionally autoseeds the slot
// claim by passing ?admin_join=N in the URL.
//
// king.html doesn't need ANY changes — it already supports the auto-
// signin flow via localStorage. We just hand it credentials. The
// `admin_join` URL param is consumed by a tiny shim we'll add in the
// next iteration; for v1, the admin still has to manually click "I GOT
// NEXT" + wait for promotion.

window.openBackdoor = function() {
  const username = (localStorage.getItem(USERNAME_KEY) || 'trisdog').toLowerCase();
  const token = localStorage.getItem(TOKEN_KEY);
  if (!token) {
    toast('error', 'BACKDOOR', 'no admin token — re-login first');
    return;
  }
  // Seed the spectator app's auth keys so king.html auto-signs in.
  // We DON'T overwrite if the user already has spectator credentials
  // — only seed if missing.
  if (!localStorage.getItem('nobd_username')) {
    localStorage.setItem('nobd_username', username);
  }
  if (!localStorage.getItem('nobd_db_token')) {
    localStorage.setItem('nobd_db_token', token);
  }
  // Open king.html in a new tab.
  const url = '/king.html?admin=1';
  window.open(url, '_blank', 'noopener,noreferrer');
  toast('info', 'BACKDOOR', `opened king.html as ${username.toUpperCase()}`);
};

window.backdoorJoinSlot = async function(slot) {
  // Same as openBackdoor but ALSO promotes the admin user directly into
  // the slot via the queue/promote path. We do this by:
  //   1. CREATE a queue row for the admin (using admin SQL via a small
  //      relay endpoint — or directly via the new /players API).
  //   2. PROMOTE that row to the target slot.
  //   3. Open king.html which will auto-sign-in and find itself in slot.
  //
  // For v1 simplicity we just kick the slot first to make sure it's
  // free, then open king.html — the admin can hit "I GOT NEXT" inside
  // the spectator app and the auto-promote will pick them up. A future
  // iteration can wire the full claim chain.
  const username = (localStorage.getItem(USERNAME_KEY) || 'trisdog').toLowerCase();
  const token = localStorage.getItem(TOKEN_KEY);
  if (!token) {
    toast('error', 'BACKDOOR', 'no admin token — re-login first');
    return;
  }
  if (!confirm(`Take P${slot + 1} as admin? Any current occupant will be kicked.`)) return;

  toast('info', 'BACKDOOR', `claiming P${slot + 1}…`);
  try {
    // 1. Kick the existing occupant if any
    await apiJson('/players/kick', {
      method: 'POST',
      body: JSON.stringify({ slot }),
    });
    // 2. Seed the spectator credentials and open king.html
    if (!localStorage.getItem('nobd_username')) {
      localStorage.setItem('nobd_username', username);
    }
    if (!localStorage.getItem('nobd_db_token')) {
      localStorage.setItem('nobd_db_token', token);
    }
    const url = `/king.html?admin=1&admin_join=${slot}`;
    window.open(url, '_blank', 'noopener,noreferrer');
    toast('success', 'BACKDOOR', `P${slot + 1} cleared, opening king.html — click "I GOT NEXT" to take the slot`);
    setTimeout(refreshPlayers, 400);
  } catch (e) {
    if (e.message !== 'auth') toast('error', 'BACKDOOR', e.message);
  }
};

// ----------------------------------------------------------------------------
// Preview controls
// ----------------------------------------------------------------------------

window.reloadPreview = function() {
  const iframe = document.getElementById('previewFrame');
  if (!iframe) return;
  // Force reload by re-setting src (changing search param to bust any cache)
  const u = new URL(iframe.src, window.location.href);
  u.searchParams.set('t', Date.now().toString());
  iframe.src = u.pathname + u.search;
  toast('info', 'PREVIEW', 'reloaded');
};

let crtEnabled = false;
window.toggleCRT = function() {
  crtEnabled = !crtEnabled;
  const wrap = document.getElementById('previewWrap');
  const btn = document.getElementById('crtToggle');
  if (crtEnabled) {
    wrap.classList.add('crt-on');
    btn.textContent = 'CRT: ON';
    btn.classList.add('active');
  } else {
    wrap.classList.remove('crt-on');
    btn.textContent = 'CRT: OFF';
    btn.classList.remove('active');
  }
  // Persist preference
  localStorage.setItem('overlord_crt', crtEnabled ? '1' : '0');
};

// ----------------------------------------------------------------------------
// Keyboard shortcuts
// ----------------------------------------------------------------------------

const SHORTCUTS = [
  { key: 'r', desc: 'Restart maplecast-headless service', fn: () => restartService() },
  { key: 's', desc: 'Save current state to selected slot', fn: () => saveCurrentTo(parseInt(document.getElementById('newSlotNum').value, 10)) },
  { key: 'p', desc: 'Reload live preview iframe', fn: () => window.reloadPreview() },
  { key: 'c', desc: 'Toggle CRT scanline overlay', fn: () => window.toggleCRT() },
  { key: 'l', desc: 'Refresh log tail', fn: () => reloadLogs() },
  { key: 'f', desc: 'Refresh savestate list', fn: () => refreshSavestates() },
  { key: 'g', desc: 'Refresh config (reload from disk)', fn: () => reloadConfig() },
  { key: '?', desc: 'Show this help', fn: () => toggleKeyhelp() },
  { key: 'Escape', desc: 'Close help / dismiss modal', fn: () => closeKeyhelp() },
];

function buildKeyhelp() {
  if (document.getElementById('keyhelpModal')) return;
  const modal = document.createElement('div');
  modal.id = 'keyhelpModal';
  modal.className = 'keyhelp-modal';
  modal.innerHTML = `
    <div class="keyhelp-card">
      <div class="keyhelp-title">⌨ KEYBOARD SHORTCUTS</div>
      ${SHORTCUTS.filter(s => s.key !== 'Escape').map(s =>
        `<div class="keyhelp-row"><span class="keyhelp-key">${escapeHtml(s.key.toUpperCase())}</span><span class="keyhelp-desc">${escapeHtml(s.desc)}</span></div>`
      ).join('')}
      <div class="keyhelp-close-hint">press <strong>ESC</strong> or <strong>?</strong> to close</div>
    </div>`;
  modal.addEventListener('click', (e) => {
    if (e.target === modal) closeKeyhelp();
  });
  document.body.appendChild(modal);
}

function toggleKeyhelp() {
  buildKeyhelp();
  const m = document.getElementById('keyhelpModal');
  m.classList.toggle('open');
}

function closeKeyhelp() {
  const m = document.getElementById('keyhelpModal');
  if (m) m.classList.remove('open');
}

function handleKeydown(e) {
  // Don't intercept when the user is typing in an input/textarea
  const tag = (e.target.tagName || '').toLowerCase();
  if (tag === 'input' || tag === 'textarea' || e.target.isContentEditable) return;
  // Don't intercept modifier-combos (let browser handle Ctrl+R, etc.)
  if (e.ctrlKey || e.metaKey || e.altKey) return;

  const sc = SHORTCUTS.find(s => s.key === e.key.toLowerCase() || s.key === e.key);
  if (sc) {
    e.preventDefault();
    sc.fn();
  }
}

// ----------------------------------------------------------------------------
// Boot
// ----------------------------------------------------------------------------

function boot() {
  if (!getToken()) {
    logoutAndRedirect();
    return;
  }
  document.getElementById('topUsername').textContent = (localStorage.getItem(USERNAME_KEY) || '?').toUpperCase();

  // Wire config editor's dirty indicator
  document.getElementById('configText').addEventListener('input', updateConfigDirty);

  // Restore CRT toggle preference
  if (localStorage.getItem('overlord_crt') === '1') {
    window.toggleCRT();
  }

  // Keyboard shortcuts
  document.addEventListener('keydown', handleKeydown);

  // Initial loads
  startStatusPoll();
  refreshSavestates();
  refreshPlayers();
  reloadConfig();
  reloadLogs();

  // Periodic refreshes
  setInterval(reloadLogs, 5000);
  setInterval(refreshPlayers, 3000);

  // First-visit hint about the keyboard shortcuts
  if (!localStorage.getItem('overlord_seen_shortcuts')) {
    setTimeout(() => {
      toast('info', 'TIP', 'Press ? for keyboard shortcuts.', 6000);
      localStorage.setItem('overlord_seen_shortcuts', '1');
    }, 2000);
  }
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', boot);
} else {
  boot();
}
