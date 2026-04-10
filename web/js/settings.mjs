// ============================================================================
// SETTINGS.MJS — Video settings panel, CSS post-processing, presets
// ============================================================================

import { state } from './state.mjs';

// Compatibility shim — the old standalone settings panel was merged into the
// unified diagnostics modal (Phase B redesign, 2026-04-08). Anything that
// previously called toggleSettings() should now open the GRAPHICS tab of the
// unified modal. Keeping this export so the existing window.toggleSettings
// binding in king.html and any other callers don't break.
import * as _diag from './diagnostics.mjs';
export function toggleSettings() {
  // Open the modal if closed; switch to the GRAPHICS tab regardless.
  const el = document.getElementById('diagOverlay');
  if (!el) return;
  const wasOpen = el.classList.contains('open');
  if (!wasOpen) {
    _diag.toggleDiag();  // opens it
  }
  // Always force-select the GRAPHICS tab when this entry point is used
  _diag.selectDiagTab?.('graphics');
}

// === CSS Post-Processing ===

export function applyCSSEffects() {
  const canvas = document.getElementById('game-canvas');
  const filters = [];

  const bright = document.getElementById('css_bright').value;
  const contrast = document.getElementById('css_contrast').value;
  const sat = document.getElementById('css_sat').value;
  document.getElementById('css_bright_val').textContent = bright + '%';
  document.getElementById('css_contrast_val').textContent = contrast + '%';
  document.getElementById('css_sat_val').textContent = sat + '%';

  filters.push('brightness(' + (bright / 100) + ')');
  filters.push('contrast(' + (contrast / 100) + ')');
  filters.push('saturate(' + (sat / 100) + ')');

  if (document.getElementById('css_sharpen').checked) filters.push('contrast(1.1)');
  if (document.getElementById('css_bloom').checked) filters.push('brightness(1.08)');

  canvas.style.filter = filters.join(' ');
  canvas.style.imageRendering = document.getElementById('css_smooth').checked ? 'auto' : 'pixelated';

  // Scanline overlay
  const screen = document.getElementById('game-screen');
  let scanEl = document.getElementById('scanline-css-overlay');
  if (document.getElementById('css_scanlines').checked) {
    if (!scanEl) {
      scanEl = document.createElement('div');
      scanEl.id = 'scanline-css-overlay';
      scanEl.style.cssText = 'position:absolute;top:0;left:0;right:0;bottom:0;pointer-events:none;z-index:15;' +
        'background:repeating-linear-gradient(0deg,transparent,transparent 1px,rgba(0,0,0,0.15) 1px,rgba(0,0,0,0.15) 3px);';
      screen.appendChild(scanEl);
    }
    scanEl.style.display = 'block';
  } else if (scanEl) { scanEl.style.display = 'none'; }

  // Vignette overlay
  let vigEl = document.getElementById('vignette-css-overlay');
  if (document.getElementById('css_vignette').checked) {
    if (!vigEl) {
      vigEl = document.createElement('div');
      vigEl.id = 'vignette-css-overlay';
      vigEl.style.cssText = 'position:absolute;top:0;left:0;right:0;bottom:0;pointer-events:none;z-index:16;' +
        'background:radial-gradient(ellipse at center,transparent 60%,rgba(0,0,0,0.5) 100%);';
      screen.appendChild(vigEl);
    }
    vigEl.style.display = 'block';
  } else if (vigEl) { vigEl.style.display = 'none'; }

  // CRT curvature
  canvas.style.borderRadius = document.getElementById('css_curvature').checked ? '10px' : '';

  saveRenderSettings();
}

// === Presets ===

function applyWasmOpts(resolution, aniso, texfilter, layers, fog, modvol, pstrip) {
  document.getElementById('opt_resolution').value = resolution;
  document.getElementById('opt_aniso').value = aniso;
  document.getElementById('opt_texfilter').value = texfilter;
  document.getElementById('opt_layers').value = layers;
  document.getElementById('opt_fog').checked = fog;
  document.getElementById('opt_modvol').checked = modvol;
  document.getElementById('opt_pstrip').checked = pstrip;
  if (state.setOpt) {
    state.setOpt(0, resolution); state.setOpt(5, aniso); state.setOpt(6, texfilter);
    state.setOpt(7, layers); state.setOpt(2, fog ? 1 : 0);
    state.setOpt(3, modvol ? 1 : 0); state.setOpt(4, pstrip ? 1 : 0);
  }
}

function applyCssOpts(scanlines, bloom, sharpen, smooth, vignette, curvature, bright, contrast, sat) {
  document.getElementById('css_scanlines').checked = scanlines;
  document.getElementById('css_bloom').checked = bloom;
  document.getElementById('css_sharpen').checked = sharpen;
  document.getElementById('css_smooth').checked = smooth;
  document.getElementById('css_vignette').checked = vignette;
  document.getElementById('css_curvature').checked = curvature;
  document.getElementById('css_bright').value = bright;
  document.getElementById('css_contrast').value = contrast;
  document.getElementById('css_sat').value = sat;
  applyCSSEffects();
}

export function presetMaxPerformance() {
  applyWasmOpts(480, 1, 0, 4, false, false, false);
  applyCssOpts(false, false, false, false, false, false, 100, 100, 100);
}

export function presetMaxQuality() {
  applyWasmOpts(1920, 16, 2, 32, true, true, true);
  applyCssOpts(false, true, true, true, false, false, 105, 110, 120);
}

export function presetArcade() {
  applyWasmOpts(480, 1, 0, 8, true, false, false);
  applyCssOpts(true, true, false, false, true, true, 110, 120, 120);
}

// === Save/Load ===

export function saveRenderSettings() {
  const s = {
    resolution: document.getElementById('opt_resolution').value,
    aniso: document.getElementById('opt_aniso').value,
    texfilter: document.getElementById('opt_texfilter').value,
    layers: document.getElementById('opt_layers').value,
    fog: document.getElementById('opt_fog').checked,
    modvol: document.getElementById('opt_modvol').checked,
    pstrip: document.getElementById('opt_pstrip').checked,
    scanlines: document.getElementById('css_scanlines').checked,
    bloom: document.getElementById('css_bloom').checked,
    sharpen: document.getElementById('css_sharpen').checked,
    smooth: document.getElementById('css_smooth').checked,
    vignette: document.getElementById('css_vignette').checked,
    curvature: document.getElementById('css_curvature').checked,
    bright: document.getElementById('css_bright').value,
    contrast: document.getElementById('css_contrast').value,
    sat: document.getElementById('css_sat').value,
  };
  localStorage.setItem('maplecast_render_settings', JSON.stringify(s));
}

export function loadRenderSettings() {
  const raw = localStorage.getItem('maplecast_render_settings');
  if (!raw) return false;
  try {
    const s = JSON.parse(raw);
    document.getElementById('opt_resolution').value = s.resolution || '480';
    document.getElementById('opt_aniso').value = s.aniso || '1';
    document.getElementById('opt_texfilter').value = s.texfilter || '0';
    document.getElementById('opt_layers').value = s.layers || '4';
    document.getElementById('opt_fog').checked = !!s.fog;
    document.getElementById('opt_modvol').checked = !!s.modvol;
    document.getElementById('opt_pstrip').checked = !!s.pstrip;
    document.getElementById('css_scanlines').checked = !!s.scanlines;
    document.getElementById('css_bloom').checked = !!s.bloom;
    document.getElementById('css_sharpen').checked = !!s.sharpen;
    document.getElementById('css_smooth').checked = !!s.smooth;
    document.getElementById('css_vignette').checked = !!s.vignette;
    document.getElementById('css_curvature').checked = !!s.curvature;
    document.getElementById('css_bright').value = s.bright || 100;
    document.getElementById('css_contrast').value = s.contrast || 100;
    document.getElementById('css_sat').value = s.sat || 100;
    return true;
  } catch (e) { return false; }
}

export function applyLoadedSettings() {
  if (state.setOpt) {
    state.setOpt(0, parseInt(document.getElementById('opt_resolution').value));
    state.setOpt(5, parseInt(document.getElementById('opt_aniso').value));
    state.setOpt(6, parseInt(document.getElementById('opt_texfilter').value));
    state.setOpt(7, parseInt(document.getElementById('opt_layers').value));
    state.setOpt(2, document.getElementById('opt_fog').checked ? 1 : 0);
    state.setOpt(3, document.getElementById('opt_modvol').checked ? 1 : 0);
    state.setOpt(4, document.getElementById('opt_pstrip').checked ? 1 : 0);
  }
  applyCSSEffects();
}
