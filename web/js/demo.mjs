// ============================================================================
// DEMO.MJS — Demo match simulation, overlays, keyboard shortcuts
// ============================================================================

export function showNewChallenger(name) {
  const overlay = document.getElementById('challengerOverlay');
  document.getElementById('challengerName').textContent = name;
  overlay.classList.add('active');
  setTimeout(() => overlay.classList.remove('active'), 3000);
}

export function showMatchResults() {
  document.getElementById('matchResults').classList.add('active');
  setTimeout(() => document.getElementById('matchResults').classList.remove('active'), 5000);
}

export function startDemoMatch() {
  document.getElementById('idleScreen').style.display = 'none';
  document.getElementById('matchHud').classList.add('active');

  let p1h = 100, p2h = 100;
  const iv = setInterval(() => {
    const attacker = Math.random() > 0.45 ? 'p1' : 'p2';
    const dmg = Math.floor(Math.random() * 8) + 2;
    if (attacker === 'p1') p2h = Math.max(0, p2h - dmg);
    else p1h = Math.max(0, p1h - dmg);

    document.getElementById('hudP1Health').style.width = p1h + '%';
    document.getElementById('hudP2Health').style.width = p2h + '%';

    const t = document.getElementById('hudTimer');
    const tv = parseInt(t.textContent);
    if (tv > 0) t.textContent = tv - 1;

    if (Math.random() > 0.7) {
      const combo = document.getElementById('hudCombo');
      combo.textContent = (Math.floor(Math.random() * 30) + 2) + ' HITS!';
      combo.classList.add('visible');
      setTimeout(() => combo.classList.remove('visible'), 800);
    }

    if (p1h <= 0 || p2h <= 0) {
      clearInterval(iv);
      document.getElementById('matchHud').classList.remove('active');
      showMatchResults();
      setTimeout(() => document.getElementById('idleScreen').style.display = 'flex', 5500);
    }
  }, 500);
}

export function setupKeyboardShortcuts() {
  document.addEventListener('keydown', e => {
    if (e.target.tagName === 'INPUT') return;
    if (e.key === 'd') startDemoMatch();
    if (e.key === 'c') showNewChallenger('NEW_PLAYER');
  });
}
