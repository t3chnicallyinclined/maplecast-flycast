#include "hw/pvr/ta.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/Renderer_if.h"

struct norend : Renderer
{
	bool Init() override {
		return true;
	}
	void Term() override { }

	void Process(TA_context* ctx) override {
		rendContext = &ctx->rend;
		ta_parse(ctx, true);
	}

	bool Render() override {
		return !rendContext->isRTT;
	}
	void RenderFramebuffer(const FramebufferInfo& info) override {
		rendContext = nullptr;
	}

	rend_context *rendContext;
};

Renderer *rend_norend() {
	return new norend();
}

// Predict-live re-sim render-skip (flycast-internals-expert, 2026-07-24).
// Identical to norend EXCEPT Process() skips ta_parse — the O(TA) cost that spikes
// the frame budget when a mispredict re-sims ~20 frames in one displayed 16.6ms tick.
//
// Determinism-SAFE for the confHash gate (= gameStateRegionHash, maplecast_rollback.cpp
// :852-863, which hashes ONLY guest main RAM mem_b: 6 char structs + gs page + frame ctr
// + fight tick). ta_parse (ta_vtx.cpp:1262-1330) writes ONLY host render structures —
// vd_rc == ctx->rend (verts/global_param_*/render_passes/fb_*_CLIP) and BaseTAParser
// statics — and only READS VRAM via pvr_read32p; it never writes mem_b, so the confHash
// region is byte-identical whether the parse runs or not. It also cannot perturb guest
// state indirectly: the SH-4 never reads ctx->rend or the parser statics back, and the
// scheduler is cycle-based (RENDER_DONE is armed by scheduleRenderDone in
// rend_start_render BEFORE Process — Renderer_if.cpp:602 — so its timing is independent
// of the parse's wall-clock cost). GetTexture is already a no-op here (base returns
// nullptr) so no texcache work is skipped — only the parse.
//
// We STILL set rendContext = &ctx->rend so Render() (which derefs rendContext->isRTT)
// and the whole QueueRender -> Render -> Present -> Stop boundary fire exactly as in the
// norend server path, keeping the byte-proven present()->Stop frame edge intact.
struct resim_norend : norend
{
	void Process(TA_context* ctx) override {
		rendContext = &ctx->rend;   // Render() derefs this; O(1)
		// intentionally NO ta_parse(ctx, true) — the invisible re-sim frame is never
		// presented; the visible head frame renders via the restored GL renderer.
	}
};

Renderer *rend_resim_norend() {
	return new resim_norend();
}
