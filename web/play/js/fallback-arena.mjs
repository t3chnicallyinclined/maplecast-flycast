/* ============================================================================
   fallback-arena.mjs — the neon "arena energy" animation shown in the cabinet
   screen while the real WebGPU render warms up (or on connect failure).
   Purely decorative, self-contained, reduced-motion aware. No deps.
   startFallbackArena(canvas) -> { stop() }
   ============================================================================ */
export function startFallbackArena(canvas) {
  const ctx = canvas.getContext('2d');
  const reduce = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  let W = 0, H = 0, raf = 0, alive = true;

  function resize() {
    const r = canvas.getBoundingClientRect();
    W = r.width; H = r.height;
    canvas.width = Math.max(1, W * dpr);
    canvas.height = Math.max(1, H * dpr);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }
  resize();
  window.addEventListener('resize', resize);

  const sparks = [];
  let flash = 0, t = 0;
  function spawn() {
    const side = Math.random() < 0.5 ? -1 : 1;
    sparks.push({
      x: W * 0.5 + side * (W * 0.1 + Math.random() * W * 0.16),
      y: H * (0.55 + Math.random() * 0.28),
      vx: side * (0.2 + Math.random() * 0.6),
      vy: -(0.5 + Math.random() * 1.4),
      life: 1, col: side < 0 ? 'mag' : 'cyan', s: 1 + Math.random() * 2.2,
    });
  }

  function draw() {
    if (!alive) return;
    t += 1;
    ctx.clearRect(0, 0, W, H);

    const g = ctx.createLinearGradient(0, H * 0.45, 0, H);
    g.addColorStop(0, 'rgba(10,7,20,0)');
    g.addColorStop(1, 'rgba(24,12,44,0.9)');
    ctx.fillStyle = g; ctx.fillRect(0, H * 0.45, W, H * 0.55);

    const hz = H * 0.56, cx = W * 0.5;
    ctx.lineWidth = 1;
    for (let i = -8; i <= 8; i++) {
      ctx.beginPath();
      ctx.strokeStyle = i === 0 ? 'rgba(35,227,255,0.2)' : 'rgba(120,80,200,0.13)';
      ctx.moveTo(cx + i * 26, hz); ctx.lineTo(cx + i * (W * 0.5) / 4, H); ctx.stroke();
    }
    for (let j = 1; j <= 7; j++) {
      const yy = hz + Math.pow(j / 7, 2.1) * (H - hz);
      ctx.beginPath(); ctx.strokeStyle = 'rgba(120,80,200,0.12)';
      ctx.moveTo(0, yy); ctx.lineTo(W, yy); ctx.stroke();
    }
    const aura = (x, col, pow) => {
      const rg = ctx.createRadialGradient(x, H * 0.6, 0, x, H * 0.6, W * 0.28 * pow);
      if (col === 'mag') { rg.addColorStop(0, 'rgba(255,45,149,0.42)'); rg.addColorStop(1, 'rgba(255,45,149,0)'); }
      else { rg.addColorStop(0, 'rgba(35,227,255,0.36)'); rg.addColorStop(1, 'rgba(35,227,255,0)'); }
      ctx.fillStyle = rg; ctx.fillRect(0, 0, W, H);
    };
    const b = 0.9 + Math.sin(t * 0.03) * 0.12;
    aura(W * 0.34, 'mag', b); aura(W * 0.66, 'cyan', 1.9 - b);

    const core = ctx.createRadialGradient(cx, H * 0.55, 0, cx, H * 0.55, 90 + flash * 120);
    core.addColorStop(0, 'rgba(255,255,255,' + (0.16 + flash * 0.5) + ')');
    core.addColorStop(0.4, 'rgba(255,120,200,' + (0.1 + flash * 0.3) + ')');
    core.addColorStop(1, 'rgba(255,255,255,0)');
    ctx.fillStyle = core; ctx.fillRect(0, 0, W, H);

    for (let k = sparks.length - 1; k >= 0; k--) {
      const p = sparks[k];
      p.x += p.vx; p.y += p.vy; p.vy += 0.012; p.life -= 0.012;
      if (p.life <= 0) { sparks.splice(k, 1); continue; }
      ctx.globalAlpha = Math.max(0, p.life);
      ctx.fillStyle = p.col === 'mag' ? '#ff2d95' : '#23e3ff';
      ctx.shadowBlur = 8; ctx.shadowColor = ctx.fillStyle;
      ctx.fillRect(p.x, p.y, p.s, p.s);
    }
    ctx.globalAlpha = 1; ctx.shadowBlur = 0;

    if (!reduce) {
      if (t % 3 === 0) spawn();
      if (Math.random() < 0.006 && flash < 0.05) flash = 1;
      flash *= 0.9;
      raf = requestAnimationFrame(draw);
    }
  }

  for (let q = 0; q < 20; q++) spawn();
  if (reduce) { for (let m = 0; m < 40; m++) sparks.forEach(p => { p.x += p.vx; p.y += p.vy; p.life -= 0.006; }); draw(); }
  else raf = requestAnimationFrame(draw);

  return {
    stop() {
      alive = false;
      cancelAnimationFrame(raf);
      window.removeEventListener('resize', resize);
      canvas.style.display = 'none';
    },
  };
}
