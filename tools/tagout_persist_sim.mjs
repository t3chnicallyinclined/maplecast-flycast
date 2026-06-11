// tagout_persist_sim.mjs — numeric simulation of SpriteGPU.render()'s present
// decision (v12 persist-on-empty + the new persist-on-collapse / hold-seed) across
// a TAG-OUT frame sequence. Asserts the INVARIANT: once the canvas has shown
// character bodies, it NEVER presents a blank (all-black) frame through a tag-out.
//
// This mirrors the EXACT branch logic in web/webgpu/sprite-gpu.mjs::render():
//   persist (v12)  : willDraw == 0 -> early return, swap-chain re-presents last frame.
//   collapse (new) : charContent(n+ni)==0 && willDraw>0 && holdValid
//                    -> seed offscreen from holdTex (loadOp:'load') -> bodies persist.
//   snapshot       : charContent>0 -> copy offscreen into holdTex, holdValid=true.
// We model the visible canvas as either "bodies" (good), "held bodies (+spark)"
// (good), or "blank" (BAD) and assert no BAD frame after the first good frame.
//
// Run: node tools/tagout_persist_sim.mjs   (exit 0 = PASS)

function simulate(frames, { persistCollapse = true, persistEmpty = true } = {}) {
  // GPU state mirrored from SpriteGPU
  let holdValid = false;          // a body frame has been snapshotted
  let swapChainLast = 'blank';    // what the opaque swap-chain currently holds
  const presented = [];           // visible canvas per frame ('bodies'|'held'|'blank')

  for (const f of frames) {
    const n = f.n | 0, ni = f.ni | 0, sn = f.sn | 0, fx = f.fx | 0;
    const willDraw = n + ni + sn + fx;
    const charContent = n + ni;

    // v12: fully empty -> skip GPU pass, swap-chain re-presents last frame.
    if (persistEmpty && willDraw === 0) {
      presented.push(swapChainLast);   // browser holds the prior present
      continue;
    }

    // new: body-collapse with surviving sparks/fx -> seed from hold.
    const seedFromHold = persistCollapse && charContent === 0 && willDraw > 0 && holdValid;

    let visible;
    if (charContent > 0) {
      visible = 'bodies';            // cleared + drew real bodies
      holdValid = true;              // snapshot taken this frame
    } else if (seedFromHold) {
      visible = 'held';              // bodies loaded from holdTex, sparks/fx over them
    } else {
      // cleared, only sparks/fx (or nothing valid to seed) -> bodies gone.
      visible = (sn + fx > 0) ? 'sparkonly' : 'blank';
    }
    swapChainLast = visible;
    presented.push(visible);
  }
  return presented;
}

function assertNoBlank(label, frames, opts) {
  const out = simulate(frames, opts);
  const firstGood = out.findIndex(v => v === 'bodies');
  let bad = -1;
  if (firstGood >= 0) {
    for (let i = firstGood; i < out.length; i++) {
      // After bodies have appeared, a 'blank' OR a 'sparkonly' (bodies vanished
      // under a spark) is a visible blank-out of the figure -> FAIL.
      if (out[i] === 'blank' || out[i] === 'sparkonly') { bad = i; break; }
    }
  }
  const ok = bad < 0;
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${label}`);
  console.log(`      frames: [${out.join(', ')}]`);
  if (!ok) console.log(`      >>> BLANK/sparkonly at frame ${bad} (after first body @${firstGood})`);
  return ok;
}

// ---- Scenario 1: tag-out where the outgoing body drops to 0 but a hit-spark from
//      the tag-in attack is still alive (the reported tag-out blank). The incoming
//      char's atlas isn't loaded for 3 frames (charContent stays 0), spark lingers.
const tagOutSpark = [
  { n: 1 },                       // outgoing point char drawing
  { n: 1, sn: 1 },                // hit lands (spark spawns) — body still up
  { n: 0, sn: 1 },                // TAG-OUT: body gone, spark alive, atlas loading
  { n: 0, sn: 1 },                // still loading
  { n: 0, sn: 1 },                // still loading
  { n: 1 },                       // incoming atlas landed -> body draws
];

// ---- Scenario 2: clean tag-out, no spark — fully empty transition frames.
const tagOutClean = [
  { n: 1 },
  { n: 0 },                       // empty transition (v12 persist holds)
  { n: 0 },
  { n: 1 },
];

// ---- Scenario 3: tag-out with a lingering EFFECT quad (fx) instead of a spark.
const tagOutFx = [
  { n: 1 },
  { n: 0, fx: 2 },                // body gone, super beam fx alive
  { n: 0, fx: 2 },
  { n: 1 },
];

let allOk = true;
allOk &= assertNoBlank('tag-out + lingering hit-spark', tagOutSpark);
allOk &= assertNoBlank('tag-out clean (empty frames)',  tagOutClean);
allOk &= assertNoBlank('tag-out + lingering effect quad', tagOutFx);

console.log('\n--- control: collapse DISABLED reproduces the blank (proves the guard is load-bearing) ---');
const ctrl = simulate(tagOutSpark, { persistCollapse: false });
const ctrlBlank = ctrl.slice(2, 5).every(v => v === 'sparkonly');
console.log(`${ctrlBlank ? 'OK' : 'XX'}  collapse=false -> frames 2..4 = [${ctrl.slice(2,5).join(', ')}] (bodies vanish, as before)`);

if (allOk && ctrlBlank) { console.log('\n=== TAG-OUT PERSIST: PASS ==='); process.exit(0); }
console.log('\n=== TAG-OUT PERSIST: FAIL ==='); process.exit(1);
