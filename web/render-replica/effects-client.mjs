// ============================================================================
// Render-Replica — PURE-EFFECT render path (FAITHFUL, disasm-traced).
//
// HISTORY / CORRECTION (2026-06-14): this module used to draw pure effects with a
// DIRECTORY-BINDING HEURISTIC — it read each effect node's GFX base node+0x15C, treated
// it as a pointer INTO the Effect-Poly directory at *(0x0CED0008), recovered a dir index
// (gfx-dirBase)/0x10, and composited a SINGLE point-centered additive quad sized by the
// directory entry. That model was built on a MISLABEL: render_frame.c (and re_kb
// finding:replica_live_effects) named "loc_8c1294c8 the cell processor for non-body
// effects." THAT IS WRONG. Tracing the disasm:
//
//   * loc_8c1294c8 (marvelous2 bank12.asm:21885) is NOT a render routine at all — it is a
//     Duff's-device 20-byte longword MEMCPY (jump-table loc_8C12951C, 18 entries, the
//     descending mov.l @(0xNN,r2),r3 / mov.l r0,@(0xNN,r1) ladder loc_8C1294DA..loc_8C129516).
//     Its ONLY caller is load-animation loc_8c034e8c (bank03:11697-11700), which does
//     `mov 0x14,r0` (=20 bytes) and copies the 20-byte anim CELL RECORD into plmem+0x140.
//     It is the per-frame ANIMATION-CELL LOADER, not an effect submitter.
//
//   * The REAL pure-effect render path (marvelous2 bank03):
//       loc_8c030af8  (satellite/effect setup, the SAME sibling of "Render Main Sprite"
//                      loc_8c03093c) — gated 0 < node+0x3 (cat) < 5 and node+0x12C==0,
//                      runs the loc_8c122560 world->screen transform, and DEPOSITS the
//                      walker fields: anchor node+0xE0/E4, scale node+0xEC/F0, facing
//                      node+0x110, cursor node+0xDC, +0x104/0x130/0x134/0x136 — IDENTICALLY
//                      to a body. It then calls the cell fetch:
//       loc_8c034bea  (bank03:11259) reads the sel node+0x144:
//                        sel == 0xFF      -> return 0  (terminator, nothing drawn)
//                        sel &  0x8000    -> loc_8c0348c8  (per-part-SCALED walker twin)
//                        else             -> loc_8c0344d4  (THE BODY WALKER)
//       loc_8c0344d4  (the proven body walker) reads cell records from GFX2 node+0x160
//                      indexed by (sel & 0x7FFF) — first u16 = record count, then count×8B
//                      records [dx s16][dy s16][FLAGS u16 (0x4000 X-mirror / 0x8000 Y-mirror)]
//                      [sel u16] with a CUMULATIVE pen — EXACTLY as for a body.
//
//   => A pure effect is rendered by the IDENTICAL walker as a body, through the IDENTICAL
//      loc_8c030af8 setup, keyed by its OWN node+0x144 sel and node+0x160 GFX2. There is NO
//      separate "effect cell processor." The directory-binding heuristic diverged from the
//      engine on every count>1 / per-cell-offset / per-cell-flip / cumulative-pen / scaled
//      (bit15) effect, and double-drew on top of the real walker output.
//
// WHAT NOW RENDERS EFFECTS: render_frame.wasm's slot-walk (gen_walker_root.c) already routes
// EVERY cat 1..4 node through render_frame_satellite_hook -> render_object_full_satellite ->
// the transpiled loc_8c030af8 setup + the shared body walker loc_8c0344d4 (re_kb finding 23,
// validated 0.0000px vs the ASMTRACE-validated body path). So pure effects render FAITHFULLY
// in the WebGPU BODY pass, with the engine's real per-cell geometry/flip/pen — NOT here.
//
// This module is therefore RETIRED to a no-op overlay: it draws NOTHING (the heuristic
// additive quad is removed so it can't double-render or diverge). The readEffectNodes()
// diagnostic is kept (now FAITHFUL: it resolves via node+0x144 sel + node+0x160 GFX2, the
// walker's real keys) so the harness can still enumerate live effect nodes and prove the
// walker is the one true path. CITE: loc_8c1294c8 (the mislabel — a memcpy), loc_8c030af8 +
// loc_8c034bea + loc_8c0344d4 (the real path), re_kb 10_replica_live_effects (superseded).
// ============================================================================

const RAM_LO = 0x00FFFFFF;
const CHAR_SLOTS = [0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74];

// Effect-Poly bank (work.asm:39 "0ced0000 - Effect Poly"). An effect node's GFX2 (node+0x160)
// points into this bank; the cell records the walker reads live here too.
const EFX_BANK_LO   = 0x0CED0000;
const EFX_BANK_HI   = 0x0CEE0000;

// slot table (the engine's own draw list — same addrs the body slot-walk uses)
const SLOT_COUNT_BASE = 0x8C2895E0;
const SLOT_PTR_BASE   = 0x8C287DE0;
const SLOT_ROW_STRIDE = 0x180;
const SLOT_LAYERS     = 16;
const SLOT_MAX_ROW    = 0x60;

// Walker key offsets (marvelous2 bank03:10380 loc_8c0345fc..; bank03:11259 loc_8c034bea).
const OFF_CAT   = 0x03;    // category byte; loc_8c030af8 gates 0 < cat < 5
const OFF_GATE  = 0x12C;   // loc_8c030af8 cull byte (must be 0 to render)
const OFF_SEL   = 0x144;   // sprite_id / sel — loc_8c034bea's dispatch key
const OFF_GFX2  = 0x160;   // cell-record table base — the walker indexes (sel&0x7FFF)
const OFF_GFX1  = 0x15C;   // (decode key; NOT the directory pointer the heuristic assumed)
const OFF_AX    = 0xE0;    // anchor X (loc_8c030af8 deposit)
const OFF_AY    = 0xE4;    // anchor Y
const OFF_FACE  = 0x110;   // facing

export class EffectsClient {
    constructor(base) {
        this.base = base;
        this.ready = false;
        this._lastCount = 0;     // diagnostics: effect nodes enumerated last frame
    }

    // Kept for API compatibility with replay.html; there is no atlas to load anymore — the
    // effect textures come from the live GFX2 bank via the walker, not a baked dir-idx atlas.
    async load() { this.ready = true; }

    // FAITHFUL effect-node enumeration (diagnostic only — rendering happens in render_frame).
    // Resolves an effect node by the WALKER's real keys: it's a cat 1..4 node, NOT a CHAR slot,
    // with GFX2 (node+0x160) in the Effect-Poly bank, gate node+0x12C==0, and sel != 0xFF. For
    // each, it reads the walker's first u16 (the cell-record COUNT) so the harness can confirm
    // the walker WOULD emit tiles (count>0) — the proof that effects go through loc_8c0344d4.
    // `rd` = { u8, u16, u32, f32 } over guest RAM. Returns one entry per live effect node.
    readEffectNodes(rd) {
        const out = [];
        for (let L = 0; L < SLOT_LAYERS; L++) {
            const cnt = rd.u8(SLOT_COUNT_BASE + L);
            if (cnt === 0 || cnt > SLOT_MAX_ROW) continue;
            const row = SLOT_PTR_BASE + L * SLOT_ROW_STRIDE;
            for (let i = 0; i < cnt; i++) {
                const node = rd.u32(row + i * 4) >>> 0;
                if (node < 0x8C000000 || node >= 0x8D000000) continue;
                if (CHAR_SLOTS.includes(node)) continue;     // bodies render via the body hook
                const cat = rd.u8(node + OFF_CAT);
                if (cat < 1 || cat >= 5) continue;            // loc_8c030af8: 0 < cat < 5
                if (rd.u8(node + OFF_GATE) !== 0) continue;   // loc_8c030af8: node+0x12C cull
                const gfx2 = rd.u32(node + OFF_GFX2) >>> 0;
                const gfx2Lo = gfx2 & 0x0FFFFFFF;
                if (gfx2Lo < EFX_BANK_LO || gfx2Lo >= EFX_BANK_HI) continue;  // not a pure effect
                const sel = rd.u16(node + OFF_SEL);
                if (sel === 0xFF) continue;                   // loc_8c034bea: terminator
                // walker key: cell = GFX2 + *(u32)(GFX2 + (sel&0x7FFF)*4); first u16 = count
                let cellCount = -1;
                try {
                    const recOff = rd.u32(gfx2 + (sel & 0x7FFF) * 4) >>> 0;
                    cellCount = rd.u16((gfx2 + recOff) >>> 0);
                } catch (e) { /* unresolved GFX2 in this RAM image */ }
                out.push({
                    node, cat, sel, gfx2,
                    gfx1: rd.u32(node + OFF_GFX1) >>> 0,
                    scaled: (sel & 0x8000) ? 1 : 0,           // loc_8c034bea -> loc_8c0348c8
                    cellCount,                                // >0 => walker emits tiles
                    sx: rd.f32(node + OFF_AX), sy: rd.f32(node + OFF_AY),
                    face: rd.u8(node + OFF_FACE),
                });
            }
        }
        return out;
    }

    // RETIRED: effects render in the WebGPU body pass via render_frame.wasm's satellite/walker
    // path (the one true loc_8c0344d4). This overlay no longer draws — it only updates the
    // diagnostic count so the harness/UI can report how many live effect nodes the walker is
    // handling. Returns 0 (no separate quads drawn here).
    render(hostCanvas, rd) {
        if (!rd) return 0;
        this._lastCount = this.readEffectNodes(rd).length;
        return 0;
    }

    lastCount() { return this._lastCount; }
}
