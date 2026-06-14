// ============================================================================
// Render-Replica PHASE 3 — EFFECTS pass (hitsparks / electric / energy).
//
// The body+satellite render (render_frame.wasm walker loc_8c0344d4) emits ZERO tiles
// for a PURE-effect pool node (cat 1..4 with GFX base in the shared "Effect Poly" bank
// 0x0CED0000 and NO body GFX2 cell records) — see render_frame.c render_object_full_ex,
// which names the unimplemented "loc_8c1294c8 cell processor for non-body effects". This
// module is that missing path, done CLIENT-side (like the stage + HUD passes): it reads the
// effect-pool nodes straight out of the shipped RAM image and draws each as one additive
// textured quad from a LOCAL effects atlas. NO server change, NO per-frame effect wire — the
// effect node STATE is already in the replica read-set ("slot_cnt"/"slot_ptr"/"objpool"
// regions, maplecast_replica_live.cpp), and the effect TEXTURES are a static ROM asset baked
// offline (tools/build_replica_effects_atlas.py).
//
// GROUNDED BINDING (NOT the unproven sprite_id guess — fx_atlas.json keymap note):
//   The Effect-Poly DIRECTORY lives at *(0x0CED0008) (a dir of 0x10-byte entries, each
//   {e0 = w | h<<16, e4 = fmt, e8 = texelPtr}). An effect node's GFX base node+0x15C IS a
//   pointer to its directory entry, so the directory index is exact arithmetic:
//       dirBase = *(0x0CED0008)
//       idx     = (node+0x15C - dirBase) / 0x10
//   The quad SIZE is the directory entry's e0 (w,h); the ANCHOR is node+0xE0/E4 (the same
//   own-origin anchor the bodies/satellites use, loc_8c030af8 deposits it identically), and
//   the quad is point-centered on it (dx=-w/2, dy=-h/2) — matching the offline fx_atlas
//   convention (tools/reshape_fx_atlas.py §6). All of dirBase, the directory, and the slot
//   table read directly from the client RAM image (the 0x0C... effect bank is the area-3
//   16MB RAM mirror the replica ships as "ram16"). VERIFIED: the directory read out of a
//   live .mcrr matches effects-capture/fx_atlas.json byte-for-byte (idx0=128x128 e8=cda4000…).
//
// SCOPE v1: render EVERY pure-effect node found, point-centered, additive. The MOST COMMON
// effect (contact hitspark) is just one of these directory entries; this pass renders it the
// moment an effect node appears in the stream. Per-type tuning (non-centered anchors, scale,
// frame animation) is the documented follow-up (see the dispatch plan in the handoff).
// ============================================================================

const RAM_LO = 0x00FFFFFF;
const CHAR_SLOTS = [0x8C268340, 0x8C2688E4, 0x8C268E88, 0x8C26942C, 0x8C2699D0, 0x8C269F74];

// Effect-Poly bank + directory pointer (work.asm:39 "0ced0000 - Effect Poly").
const EFX_BANK_LO   = 0x0CED0000;   // low-28-bit start of the effect bank
const EFX_BANK_HI   = 0x0CEE0000;
const EFX_DIR_PTR   = 0x0CED0008;   // *(.) = directory base (dynamic per effect-bank load)

// slot table (the engine's own draw list — same addrs readAllDrawn uses)
const SLOT_COUNT_BASE = 0x8C2895E0;
const SLOT_PTR_BASE   = 0x8C287DE0;
const SLOT_ROW_STRIDE = 0x180;
const SLOT_LAYERS     = 16;
const SLOT_MAX_ROW    = 0x60;

export class EffectsClient {
    // base = URL of the effects atlas dir (effects.json + effects.png live there)
    constructor(base) {
        this.base = base;
        this.meta = null;        // effects.json (dir[] rects keyed by idx)
        this.img = null;         // HTMLImageElement of effects.png
        this.ready = false;
        this._canvas = null;     // 2D overlay canvas (additive, above bodies, below HUD)
        this._ctx = null;
        this._host = null;
        this._lastCount = 0;     // diagnostics: effect quads drawn last frame
    }

    async load() {
        if (this.ready) return;
        const meta = await (await fetch(this.base + '/effects.json?v=fx1')).json();
        const img = new Image();
        img.src = this.base + '/effects.png?v=fx1';
        await img.decode();
        this.meta = meta; this.img = img;
        // dir[] -> map idx -> rect for O(1) lookup
        this._byIdx = {};
        for (const d of meta.dir) this._byIdx[d.idx] = d;
        this.ready = true;
    }

    // Lazily create the additive 2D overlay layered EXACTLY over the WebGPU body canvas.
    // z-order: bodies/stage (WebGPU) < EFFECTS (this, additive) < HUD (2D overlay). The HUD
    // overlay appends after this one, so it ends up on top in DOM order.
    _ensureOverlay(hostCanvas) {
        if (this._canvas && this._host === hostCanvas) return this._ctx;
        this._host = hostCanvas;
        const c = document.createElement('canvas');
        c.width = hostCanvas.width; c.height = hostCanvas.height;
        c.style.cssText = hostCanvas.style.cssText;
        c.style.background = 'transparent';
        c.style.position = 'absolute';
        c.style.pointerEvents = 'none';
        hostCanvas.style.position = 'relative';
        hostCanvas.parentElement.style.position = 'relative';
        hostCanvas.parentElement.appendChild(c);
        c.style.left = hostCanvas.offsetLeft + 'px';
        c.style.top  = hostCanvas.offsetTop + 'px';
        this._canvas = c; this._ctx = c.getContext('2d');
        return this._ctx;
    }

    // Read the live effect-pool nodes from the RAM image. `rd` = { u8, u16, u32, f32 }
    // accessors over the guest RAM (same ones replay.html builds). Returns an array of
    // { idx, w, h, sx, sy, sid, cat, xflip } — one entry per pure-effect node, resolved.
    readEffectNodes(rd) {
        const dirBase = rd.u32(EFX_DIR_PTR) >>> 0;
        const dirLo = dirBase & RAM_LO;
        // sanity: directory must point into the area-3 RAM image
        if (dirLo === 0 || dirLo >= 0x1000000) return [];
        const out = [];
        for (let L = 0; L < SLOT_LAYERS; L++) {
            const cnt = rd.u8(SLOT_COUNT_BASE + L);
            if (cnt === 0 || cnt > SLOT_MAX_ROW) continue;
            const row = SLOT_PTR_BASE + L * SLOT_ROW_STRIDE;
            for (let i = 0; i < cnt; i++) {
                const node = rd.u32(row + i * 4) >>> 0;
                if (node < 0x8C000000 || node >= 0x8D000000) continue;
                if (CHAR_SLOTS.includes(node)) continue;          // bodies render via the walker
                const gfx = rd.u32(node + 0x15C) >>> 0;
                const gfxLo = gfx & 0x0FFFFFFF;
                if (gfxLo < EFX_BANK_LO || gfxLo >= EFX_BANK_HI) continue;  // not an effect node
                // directory index = (gfx - dirBase) / 0x10  (gfx IS a dir-entry pointer)
                const off = ((gfx & RAM_LO) - dirLo);
                if (off < 0 || (off & 0xF)) continue;             // must land ON a dir entry
                const idx = off >> 4;
                // pull w/h straight from the live directory entry (authoritative size)
                const e = (dirBase + idx * 0x10) >>> 0;
                const e0 = rd.u32(e) >>> 0;
                let w = e0 & 0xffff, h = (e0 >>> 16) & 0xffff;
                if (w === 0 || h === 0 || w > 512 || h > 512) continue;  // not a live texture
                out.push({
                    idx, w, h,
                    sx: rd.f32(node + 0xE0), sy: rd.f32(node + 0xE4),
                    sid: rd.u16(node + 0x144),
                    cat: rd.u8(node + 0x03),
                    xflip: rd.u16(node + 0x130) ? 1 : 0,
                });
            }
        }
        return out;
    }

    // Render the effects pass onto a 2D overlay above `hostCanvas`. `rd` = RAM accessors.
    // Additive blend (globalCompositeOperation='lighter') matches the PVR additive list the
    // engine submits effects through (re_kb reference_mvc2_effects_bank: "additive blend,
    // already plumbed"). Returns the number of effect quads drawn (0 = nothing on screen).
    render(hostCanvas, rd) {
        if (!this.ready) return 0;
        const ctx = this._ensureOverlay(hostCanvas);
        const W = this._canvas.width, H = this._canvas.height;
        ctx.clearRect(0, 0, W, H);
        const nodes = this.readEffectNodes(rd);
        this._lastCount = nodes.length;
        if (!nodes.length) return 0;

        ctx.save();
        ctx.scale(W / this.meta.screenW, H / this.meta.screenH);  // game space -> canvas
        ctx.imageSmoothingEnabled = false;
        ctx.globalCompositeOperation = 'lighter';                // ADDITIVE (effect blend)
        for (const n of nodes) {
            const rect = this._byIdx[n.idx];
            if (!rect) continue;                                  // unknown dir idx -> skip (no guess)
            // point-centered on the node anchor (dx=-w/2, dy=-h/2), size = directory (w,h)
            const dw = n.w, dh = n.h;
            const dx = n.sx - dw / 2, dy = n.sy - dh / 2;
            if (n.xflip) {
                ctx.save();
                ctx.translate(n.sx, 0); ctx.scale(-1, 1); ctx.translate(-n.sx, 0);
                ctx.drawImage(this.img, rect.x, rect.y, rect.w, rect.h, dx, dy, dw, dh);
                ctx.restore();
            } else {
                ctx.drawImage(this.img, rect.x, rect.y, rect.w, rect.h, dx, dy, dw, dh);
            }
        }
        ctx.restore();
        return nodes.length;
    }

    lastCount() { return this._lastCount; }
}
