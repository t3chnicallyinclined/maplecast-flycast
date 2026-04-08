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

function renderStatus(s) {
  const pillEl = document.getElementById('topStatusPill');
  const pillText = document.getElementById('topStatusText');
  const dot = pillEl.querySelector('.dot');
  if (s.flycast_active) {
    pillText.textContent = 'RUNNING';
    dot.className = 'dot dot-green';
  } else {
    pillText.textContent = 'STOPPED';
    dot.className = 'dot dot-red';
  }

  document.getElementById('statusService').textContent = s.flycast_active ? 'active (running)' : 'stopped';
  document.getElementById('statusService').className = `status-val ${s.flycast_active ? 'good' : 'bad'}`;
  document.getElementById('statusPid').textContent = s.flycast_pid != null ? String(s.flycast_pid) : '—';
  document.getElementById('statusUptime').textContent = fmtUptime(s.flycast_uptime_s);
  document.getElementById('statusMem').textContent = s.flycast_mem_mb != null ? `${s.flycast_mem_mb} MB` : '—';
  document.getElementById('statusSlot').textContent = s.current_slot != null ? `slot ${s.current_slot}` : '—';
  document.getElementById('statusControlWs').textContent = s.control_ws || '—';

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
    return `<tr class="row${isCurrent}">
      <td class="slot-num">${s.slot}${currentBadge}</td>
      <td>${escapeHtml(s.filename)}</td>
      <td>${fmtBytes(s.size)}</td>
      <td>${fmtMtime(s.mtime)}</td>
      <td class="row-actions">
        <button class="btn-row btn-row-disabled" title="Hot-load disabled — see Phase A.2 known issue. Workaround: edit emu.cfg to set this slot, then RESTART SERVICE.">LOAD</button>
        <button class="btn-row" onclick="downloadSlot(${s.slot})">DOWNLOAD</button>
      </td>
    </tr>`;
  }).join('');
}

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

  // Initial loads
  startStatusPoll();
  refreshSavestates();
  reloadConfig();
  reloadLogs();

  // Periodic log refresh every 5 seconds
  setInterval(reloadLogs, 5000);
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', boot);
} else {
  boot();
}
