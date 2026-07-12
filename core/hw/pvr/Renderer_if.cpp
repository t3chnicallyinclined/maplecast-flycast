#include "Renderer_if.h"
#include "spg.h"
#include "rend/texconv.h"
#include "rend/transform_matrix.h"
#include "cfg/option.h"
#include "emulator.h"
#include "serialize.h"
#include "hw/holly/holly_intc.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_core.h"
#include "profiler/fc_profiler.h"
#include "network/ggpo.h"
#ifndef MAPLECAST_HEADLESS_BUILD
#include "network/maplecast_stream.h"
#endif
#include "network/maplecast_mirror.h"
#include "network/maplecast_control_ws.h"
#include "network/maplecast_input_server.h"
#include "network/maplecast_gamestate.h"   // MAPLECAST_BAKE sprite-stability probe
#include "network/maplecast_oracle_hook.h" // MAPLECAST_CHARQ pre-QueueRender body-quad capture
#include "network/maplecast_predict.h"     // no-render re-sim primitive (headless advance)

#include <mutex>
#include <deque>
#include <vector>
#include <set>
#include <cstdlib>

// MAPLECAST_DUMP_TA — cross-platform support
#include <cstdio>
#include <cerrno>
#include <string>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#ifdef LIBRETRO
void retro_rend_present();
void retro_resize_renderer(int w, int h, float aspectRatio);
#endif

u32 FrameCount=1;

Renderer* renderer;

static cResetEvent renderEnd;
u32 fb_w_cur = 1;
static cResetEvent vramRollback;

// direct framebuffer write detection
static bool render_called = false;
u32 fb_watch_addr_start;
u32 fb_watch_addr_end;
bool fb_dirty;

static bool pend_rend;
static bool rendererEnabled = true;

static bool presented;
static u32 fbAddrHistory[2] { 1, 1 };

class PvrMessageQueue
{
	using lock_guard = std::lock_guard<std::mutex>;

public:
	enum MessageType { NoMessage = -1, Render, RenderFramebuffer, Present, Stop };
	struct Message
	{
		Message() = default;
		Message(MessageType type, FramebufferInfo config)
			: type(type), config(config) {}

		MessageType type = NoMessage;
		FramebufferInfo config;
	};

	void enqueue(MessageType type, FramebufferInfo config = FramebufferInfo())
	{
		Message msg { type, config };
		if (config::ThreadedRendering)
		{
			// FIXME need some synchronization to avoid blinking in densha de go
			// or use !threaded rendering for emufb?
			// or read framebuffer vram on emu thread
			bool dupe;
			do {
				dupe = false;
				{
					const lock_guard lock(mutex);
					for (const auto& m : queue)
						if (m.type == type) {
							dupe = true;
							break;
						}
					if (!dupe || type == Present) {
						queue.push_back(msg);
						dupe = false;
					}
				}
				if (dupe)
				{
					if (type == Stop)
						return;
					dequeueEvent.Wait();
				}
			} while (dupe);
			enqueueEvent.Set();
		}
		else
		{
			setDefaultRoundingMode();
			// drain the queue after switching to !threaded rendering
			while (!queue.empty())
				waitAndExecute();
			execute(msg);
			Sh4cntx.restoreHostRoundingMode();
		}
	}

	bool waitAndExecute(int timeoutMs = -1)
	{
		return execute(dequeue(timeoutMs));
	}

	void reset() {
		const lock_guard lock(mutex);
		queue.clear();
	}

	void cancelEnqueue()
	{
		const lock_guard lock(mutex);
		for (auto it = queue.begin(); it != queue.end(); )
		{
			if (it->type != Render)
				it = queue.erase(it);
			else
				++it;
		}
		dequeueEvent.Set();
	}
private:
	Message dequeue(int timeoutMs = -1)
	{
		FC_PROFILE_SCOPE;

		Message msg;
		while (true)
		{
			{
				const lock_guard lock(mutex);
				if (!queue.empty())
				{
					msg = queue.front();
					queue.pop_front();
				}
			}
			if (msg.type != NoMessage) {
				dequeueEvent.Set();
				break;
			}
			if (timeoutMs == -1)
				enqueueEvent.Wait();
			else if (!enqueueEvent.Wait(timeoutMs))
				break;
		}
		return msg;
	}

	bool execute(Message msg)
	{
		switch (msg.type)
		{
		case Render:
			render();
			return true;
		case RenderFramebuffer:
			renderFramebuffer(msg.config);
			return true;
		case Present:
			present();
			return true;
		case Stop:
		case NoMessage:
		default:
			return false;
		}
	}

	void render()
	{
		FC_PROFILE_SCOPE;

		TA_context *taContext = DequeueRender();
		if (taContext == nullptr)
			return;

		if (!taContext->rend.isRTT)
		{
			int width, height;
			getScaledFramebufferSize(taContext->rend, width, height);
			taContext->rend.framebufferWidth = width;
			taContext->rend.framebufferHeight = height;
		}
		bool renderToScreen = !taContext->rend.isRTT && !config::EmulateFramebuffer;
#ifdef LIBRETRO
		if (renderToScreen)
			retro_resize_renderer(taContext->rend.framebufferWidth, taContext->rend.framebufferHeight,
					getOutputFramebufferAspectRatio());
#endif
		{
			FC_PROFILE_SCOPE_NAMED("Renderer::Process");
			// /overlord control WS: drain any queued admin commands
			// (savestate save/load, reset) before publishing TA. This
			// runs on the render thread so dc_savestate/dc_loadstate
			// are called from the right thread; the WS handler thread
			// just queues. After a load, the executor calls
			// requestSyncBroadcast() so the next serverPublish below
			// emits a fresh full SYNC and mirror clients realign.
			if (maplecast_mirror::isServer())
				maplecast_control_ws::drainCommandQueue();
			// Mirror server: capture TA commands BEFORE Process consumes them
			if (maplecast_mirror::isServer() && taContext)
				maplecast_mirror::serverPublish(taContext);

			// MAPLECAST_DUMP_TA=1 — TA-buffer dump for the determinism rig.
			// Independent of mirror server (which is Linux-only via /dev/shm).
			// Works on Windows too, so the same SH4 binary running on
			// different OSes can be cross-checked for byte-identical output.
			// Output: <MAPLECAST_DUMP_TA_DIR>/frame_NNNNNN.bin (one per TA pass).
			if (taContext) {
				static bool _ddInit = false;
				static bool _ddEnabled = false;
				static std::string _ddDir;
				static uint32_t _ddFrameNum = 0;
				if (!_ddInit) {
					const char* e = std::getenv("MAPLECAST_DUMP_TA");
					_ddEnabled = (e && *e && *e != '0');
					if (_ddEnabled) {
						const char* d = std::getenv("MAPLECAST_DUMP_TA_DIR");
						_ddDir = (d && *d) ? d : "/tmp/ta-dumps-render";
#ifdef _WIN32
						int rc = _mkdir(_ddDir.c_str());
#else
						int rc = mkdir(_ddDir.c_str(), 0755);
#endif
						printf("[TA-DUMP-RENDER] enabled — writing %s/frame_NNNNNN.bin (mkdir rc=%d errno=%d)\n",
						       _ddDir.c_str(), rc, errno);
						fflush(stdout);
					}
					_ddInit = true;
				}
				if (_ddEnabled) {
					// taContext->tad is the parsed TA accumulator. Raw bytes
					// live in tad.thd_root, current end at tad.thd_data
					// (mirrors how serverPublish computes taSize).
					const uint8_t* taData = taContext->tad.thd_root;
					size_t taSize = (size_t)(taContext->tad.thd_data - taContext->tad.thd_root);
					if (taSize > 0) {
						_ddFrameNum++;
						char path[512];
						snprintf(path, sizeof(path), "%s/frame_%06u.bin",
						         _ddDir.c_str(), _ddFrameNum);
						FILE* f = fopen(path, "wb");
						if (f) {
							fwrite(taData, 1, taSize, f);
							fclose(f);
						} else {
							static int _warnedRender = 0;
							if (_warnedRender++ < 3)
								printf("[TA-DUMP-RENDER] fopen(%s) failed: errno=%d\n", path, errno);
						}
					}
				}
			}

			// Renderer-level recording hook. Only fires when MAPLECAST_REPLAY_OUT
			// is set — keeps standalone-without-recording as a zero-overhead path
			// (no per-frame ring pushes, no replay::append mutex traffic).
			if (taContext && !maplecast_mirror::isServer()) {
				static const bool _recordHookEnabled = std::getenv("MAPLECAST_REPLAY_OUT") != nullptr;
				if (_recordHookEnabled) {
					static uint64_t _renderRecordFrame = 0;
					_renderRecordFrame++;
					maplecast_input::publishFrameTickFromGlobals(_renderRecordFrame);
				}
			}
			try {
				renderer->Process(taContext);
			} catch (...) {
				renderEnd.Set();
				rend_allow_rollback();
				FinishRender(taContext);
				throw;
			}
		}

		if (renderToScreen)
			// If rendering to texture or in full framebuffer emulation, continue locking until the frame is rendered
			renderEnd.Set();
		rend_allow_rollback();
		{
			FC_PROFILE_SCOPE_NAMED("Renderer::Render");
			try {
				renderer->Render();
			} catch (...) {
				if (!renderToScreen)
					renderEnd.Set();
				FinishRender(taContext);
				throw;
			}
		}

		if (!renderToScreen)
			renderEnd.Set();
		else if (config::DelayFrameSwapping && fb_w_cur == FB_R_SOF1)
			present();

		//clear up & free data ..
		FinishRender(taContext);
	}

	void renderFramebuffer(const FramebufferInfo& config)
	{
		FC_PROFILE_SCOPE;

#ifdef LIBRETRO
		int w, h;
		getDCFramebufferReadSize(config, w, h);
		retro_resize_renderer(w, h, getDCFramebufferAspectRatio());
#endif
		renderer->RenderFramebuffer(config);
	}

	void present()
	{
		FC_PROFILE_SCOPE;

		if (renderer->Present())
		{
			presented = true;

			// === MAPLECAST_BAKE — P0 sprite-stability probe ===
			// docs/BAKE-HARNESS-PLAN.md. After the frame is presented, grab it
			// via the backend-agnostic GetLastFrame(), crop a fixed per-slot box
			// around each point character, FNV-1a hash the crop, and log it with
			// sprite_id/position/camera. Verifies (char_id,sprite_id) -> stable
			// pixels: the same sprite at the same position+camera MUST hash
			// identically. Saves each sprite's first crop (raw RGB) for
			// eyeballing. Inert unless MAPLECAST_BAKE is set; read-only w.r.t.
			// emulation. Post-Present, so GetLastFrame's GL-state churn is
			// harmless (the next frame re-establishes state).
			{
				static int   _bkInit = 0;
				static bool  _bkOn   = false;
				static std::string _bkDir;
				static FILE* _bkCsv  = nullptr;
				static float _bkX0=0.10f,_bkY0=0.28f,_bkX1=0.50f,_bkY1=0.96f;
				static std::set<uint64_t> _bkSeen;
				static uint32_t _bkFrame = 0;
				if (!_bkInit) {
					_bkInit = 1;
					if (const char* d = std::getenv("MAPLECAST_BAKE")) {
						_bkOn  = (*d != 0 && *d != '0');
						_bkDir = d;
						if (const char* b = std::getenv("MAPLECAST_BAKE_BOX"))
							sscanf(b, "%f,%f,%f,%f", &_bkX0,&_bkY0,&_bkX1,&_bkY1);
						if (_bkOn) {
#ifdef _WIN32
							_mkdir(_bkDir.c_str());
#else
							mkdir(_bkDir.c_str(), 0755);
#endif
							_bkCsv = fopen((_bkDir + "/bake.csv").c_str(), "w");
							if (_bkCsv)
								fprintf(_bkCsv, "frame,slot,char_id,sprite_id,"
									"screen_x,screen_y,camera_x,camera_y,facing,"
									"crop_w,crop_h,crop_hash\n");
							printf("[BAKE] enabled -> %s (box %.2f,%.2f,%.2f,%.2f)\n",
								_bkDir.c_str(), _bkX0,_bkY0,_bkX1,_bkY1);
							fflush(stdout);
						}
					}
				}
				if (_bkOn && _bkCsv && renderer != nullptr) {
					_bkFrame++;
					std::vector<u8> fb; int fw=0, fh=0;
					if (renderer->GetLastFrame(fb, fw, fh)
					    && fw>0 && fh>0 && fb.size() >= (size_t)fw*fh*3) {
						maplecast_gamestate::GameState gs;
						maplecast_gamestate::readGameState(gs);
						for (int slot = 0; slot < 2; slot++) {
							const maplecast_gamestate::CharacterState& c = gs.chars[slot];
							if (!c.active) continue;
							float fx0 = (slot==1) ? (1.0f-_bkX1) : _bkX0;
							float fx1 = (slot==1) ? (1.0f-_bkX0) : _bkX1;
							int x0=(int)(fx0*fw), x1=(int)(fx1*fw);
							int y0=(int)(_bkY0*fh), y1=(int)(_bkY1*fh);
							if (x0<0) x0=0; if (y0<0) y0=0;
							if (x1>fw) x1=fw; if (y1>fh) y1=fh;
							int cw=x1-x0, ch=y1-y0;
							if (cw<=0 || ch<=0) continue;
							// FNV-1a 64 over the crop (RGB, top-down rows)
							uint64_t hh = 1469598103934665603ULL;
							for (int yy=y0; yy<y1; yy++) {
								const u8* row = &fb[((size_t)yy*fw + x0)*3];
								for (int i=0; i<cw*3; i++) { hh ^= row[i]; hh *= 1099511628211ULL; }
							}
							fprintf(_bkCsv,
								"%u,%d,%u,%u,%.2f,%.2f,%.2f,%.2f,%u,%d,%d,0x%016llX\n",
								_bkFrame, slot, c.character_id, c.sprite_id,
								c.screen_x, c.screen_y, gs.camera_x, gs.camera_y,
								c.facing_right, cw, ch, (unsigned long long)hh);
							uint64_t key = ((uint64_t)slot<<48)
								^ ((uint64_t)c.character_id<<32) ^ c.sprite_id;
							if (_bkSeen.insert(key).second) {
								char p[600];
								snprintf(p, sizeof(p), "%s/crop_s%d_c%02u_sp%04X.bin",
									_bkDir.c_str(), slot, c.character_id, c.sprite_id);
								FILE* cf = fopen(p, "wb");
								if (cf) {
									uint32_t hdr[2] = {(uint32_t)cw,(uint32_t)ch};
									fwrite(hdr, sizeof(hdr), 1, cf);
									for (int yy=y0; yy<y1; yy++)
										fwrite(&fb[((size_t)yy*fw + x0)*3], 1, (size_t)cw*3, cf);
									fclose(cf);
								}
							}
						}
						if ((_bkFrame % 60)==0) fflush(_bkCsv);
					}
				}
			}

			// MapleCast H.264/JPEG stream — disabled when mirror mode is active
			// Mirror uses maplecast_ws_server for WebSocket broadcast instead
			// if (maplecast_stream::active())
			// {
			// 	try { maplecast_stream::onFrameRendered(); } catch (...) {}
			// }
			if (!config::ThreadedRendering && !ggpo::active())
				emu.getSh4Executor()->Stop();
#ifdef LIBRETRO
			retro_rend_present();
#endif
		}
	}

	std::mutex mutex;
	cResetEvent enqueueEvent;
	cResetEvent dequeueEvent;
	std::deque<Message> queue;
};

static PvrMessageQueue pvrQueue;

bool rend_single_frame(const bool& enabled)
{
	FC_PROFILE_SCOPE;

	const int timeout = SPG_CONTROL.isPAL() ? 23 : 20;
	presented = false;
	while (enabled && !presented)
		if (!pvrQueue.waitAndExecute(timeout))
			return false;
	return true;
}

Renderer* rend_GLES2();
Renderer* rend_GL4();
Renderer* rend_norend();
Renderer* rend_Vulkan();
Renderer* rend_OITVulkan();
Renderer* rend_DirectX9();
Renderer* rend_DirectX11();
Renderer* rend_OITDirectX11();

static void rend_create_renderer()
{
	// MapleCast headless: force norend unconditionally, even on GPU builds.
	// norend's Process() runs ta_parse(ctx, true) on CPU, which is exactly
	// what serverPublish() needs (serverPublish() is called BEFORE
	// renderer->Process() in the render message loop below, so the mirror
	// wire bytes are identical to the GPU-backed path — enforced by the
	// MAPLECAST_DUMP_TA determinism rig).
	if (maplecast_mirror::isHeadless())
	{
		renderer = rend_norend();
		return;
	}
#ifdef NO_REND
	renderer	 = rend_norend();
#else
	switch (config::RendererType)
	{
	default:
#ifdef USE_OPENGL
	case RenderType::OpenGL:
		renderer = rend_GLES2();
		break;
#if !defined(GLES2) && !defined(__APPLE__)
	case RenderType::OpenGL_OIT:
		renderer = rend_GL4();
		break;
#endif
#endif
#ifdef USE_VULKAN
	case RenderType::Vulkan:
		renderer = rend_Vulkan();
		break;
	case RenderType::Vulkan_OIT:
		renderer = rend_OITVulkan();
		break;
#endif
#ifdef USE_DX9
	case RenderType::DirectX9:
		renderer = rend_DirectX9();
		break;
#endif
#ifdef USE_DX11
	case RenderType::DirectX11:
		renderer = rend_DirectX11();
		break;
	case RenderType::DirectX11_OIT:
		renderer = rend_OITDirectX11();
		break;
#endif
	}
#endif
}

bool rend_init_renderer()
{
	rendererEnabled = true;
	if (renderer == nullptr)
		rend_create_renderer();
	bool success = renderer != nullptr && renderer->Init();
	if (!success) {
		delete renderer;
		renderer = rend_norend();
		renderer->Init();
	}
	return success;
}

void rend_term_renderer()
{
	if (renderer != nullptr)
	{
		renderer->Term();
		delete renderer;
		renderer = nullptr;
	}
}

void rend_reset()
{
	FinishRender(DequeueRender());
	render_called = false;
	pend_rend = false;
	FrameCount = 1;
	fb_w_cur = 1;
	pvrQueue.reset();
	rendererEnabled = true;
	fbAddrHistory[0] = 1;
	fbAddrHistory[1] = 1;
}

void rend_start_render()
{
	render_called = true;
	pend_rend = false;

	TA_context *ctx = nullptr;
	u32 addresses[MAX_PASSES];
	int count = getTAContextAddresses(addresses);
	if (count > 0)
	{
		ctx = tactx_Pop(addresses[0]);
		if (ctx != nullptr)
		{
			TA_context *linkedCtx = ctx;
			for (int i = 1; i < count; i++)
			{
				linkedCtx->nextContext = tactx_Pop(addresses[i]);
				if (linkedCtx->nextContext != nullptr)
					linkedCtx = linkedCtx->nextContext;
				else
					INFO_LOG(PVR, "rend_start_render: Context%d @ %x not found", i, addresses[i]);
			}
		}
		else
			INFO_LOG(PVR, "rend_start_render: Context0 @ %x not found", addresses[0]);
	}
	else
		INFO_LOG(PVR, "rend_start_render: No context not found");

	scheduleRenderDone(ctx);

	if (ctx == nullptr)
		return;

	FillBGP(ctx);

	ctx->rend.isRTT = (FB_W_SOF1 & 0x1000000) != 0;
	// A2 run-ahead: sample the leg flag HERE (emu thread, synchronous) — see ta_ctx.h.
	ctx->rend.mc_hiddenLeg = maplecast_mirror::suppressActive();
	ctx->rend.mc_vframe = maplecast_mirror::currentGuestVf();   // rewind-proof vf (defect #2)
	// A2 TRACE (MAPLECAST_RUNAHEAD_TRACE=1): the decisive datum — does STARTRENDER for frame N's
	// content fire during the NEXT tick's hidden window? If PUB lines show hid=1 on preview vfs,
	// the pipelined-submit hypothesis is confirmed and publish must be CONTENT-keyed (vf), not leg-stamped.
	{
		static const bool _raTrace = std::getenv("MAPLECAST_RUNAHEAD_TRACE") != nullptr;
		if (_raTrace) {
			static uint32_t _ord = 0;
			printf("[RA-TRACE] SR  ord=%u leg=%d vf=%u\n", ++_ord,
				(int)ctx->rend.mc_hiddenLeg, ctx->rend.mc_vframe);
		}
	}
	ctx->rend.fb_W_SOF1 = FB_W_SOF1;
	ctx->rend.fb_W_CTRL.full = FB_W_CTRL.full;

	ctx->rend.ta_GLOB_TILE_CLIP = TA_GLOB_TILE_CLIP;
	ctx->rend.scaler_ctl = SCALER_CTL;
	ctx->rend.fb_X_CLIP = FB_X_CLIP;
	ctx->rend.fb_Y_CLIP = FB_Y_CLIP;
	ctx->rend.fb_W_LINESTRIDE = FB_W_LINESTRIDE.stride;

	ctx->rend.fog_clamp_min = FOG_CLAMP_MIN;
	ctx->rend.fog_clamp_max = FOG_CLAMP_MAX;

	if (!ctx->rend.isRTT)
	{
		if (FB_W_SOF1 != fbAddrHistory[0] && FB_W_SOF1 != fbAddrHistory[1])
		{
			ctx->rend.clearFramebuffer = true;
			fbAddrHistory[0] = fbAddrHistory[1];
			fbAddrHistory[1] = FB_W_SOF1;
		}
		else {
			ctx->rend.clearFramebuffer = false;
		}
		ggpo::endOfFrame();
		// MapleCast predict: no-render re-sim primitive. When a headless advance
		// is in progress, stop the SH4 at the display (non-RTT) STARTRENDER — one
		// game-frame — exactly like ggpo::endOfFrame above, but with the render
		// skipped (rend_enable_renderer(false) => QueueRender below recycles the
		// ctx, zero GPU work). scheduleRenderDone already fired (line ~600) so the
		// RENDER_DONE interrupt timing is unchanged. No-op unless a headless
		// advance is active on this (emu) thread.
		if (maplecast_predict::headlessAdvanceActive())
			emu.getSh4Executor()->Stop();
	}

	// === MAPLECAST_CHARQ — DEFINITIVE per-part body-quad capture ===============
	// QueueRender below is SINGLE-SLOT (ta_ctx.cpp:67-73): when rqueue is already busy
	// it DROPS this context (tactx_Recycle + returns false). MVC2 emits multiple
	// STARTRENDER passes per video frame; the CHARACTER pass (per-part body quads,
	// op~265/tr~2024, body-band y240-433) is the one QueueRender drops on MVC2 — only the
	// HUD/composite pass survives to DequeueRender -> render() -> serverPublish. So this,
	// right HERE (after the isRTT/rend stamp, BEFORE QueueRender can drop it), is the ONLY
	// point in the pipeline where the per-part character quads exist. Capture them.
	//
	// READ-ONLY + determinism-safe: mc_oracle_charPassCapture ta_parse(ctx,true)'s the ctx
	// exactly like norend::Process — it builds ctx->rend, never writes guest state, never
	// enqueues/recycles, never touches rqueue. The real render path re-parses for the wire.
	// Gated MAPLECAST_CHARQ + in-match (0x8C289624); a no-op otherwise -> prod unaffected.
	// Called for EVERY STARTRENDER ctx (the dropped character pass AND the surviving HUD
	// pass); the capture path is per-vframe deduped and emits only on the character pass.
	maplecast_oracle_hook::mc_oracle_charPassCapture(ctx);

	if (QueueRender(ctx))
	{
		palette_update();
		pend_rend = true;
		pvrQueue.enqueue(PvrMessageQueue::Render);
		if (!config::DelayFrameSwapping && !ctx->rend.isRTT && !config::EmulateFramebuffer)
			pvrQueue.enqueue(PvrMessageQueue::Present);
	}
}

int rend_end_render(int tag, int cycles, int jitter, void *arg)
{
	if (settings.platform.isNaomi2())
	{
		asic_RaiseInterruptBothCLX(holly_RENDER_DONE);
		asic_RaiseInterruptBothCLX(holly_RENDER_DONE_isp);
		asic_RaiseInterruptBothCLX(holly_RENDER_DONE_vd);
	}
	else
	{
		asic_RaiseInterrupt(holly_RENDER_DONE);
		asic_RaiseInterrupt(holly_RENDER_DONE_isp);
		asic_RaiseInterrupt(holly_RENDER_DONE_vd);
	}
	if (pend_rend && config::ThreadedRendering)
		renderEnd.Wait();

	return 0;
}

void rend_vblank()
{
	if (config::EmulateFramebuffer
			|| (!render_called && fb_dirty && FB_R_CTRL.fb_enable))
	{
		if (rend_is_enabled())
		{
			FramebufferInfo fbInfo;
			fbInfo.update();
			pvrQueue.enqueue(PvrMessageQueue::RenderFramebuffer, fbInfo);
			pvrQueue.enqueue(PvrMessageQueue::Present);
			if (!config::EmulateFramebuffer)
				DEBUG_LOG(PVR, "Direct framebuffer write detected");
		}
		fb_dirty = false;
	}
	render_called = false;
	check_framebuffer_write();
	emu.vblank();
}

void check_framebuffer_write()
{
	u32 fb_size = (FB_R_SIZE.fb_y_size + 1) * (FB_R_SIZE.fb_x_size + FB_R_SIZE.fb_modulus) * 4;
	fb_watch_addr_start = (SPG_CONTROL.interlace ? FB_R_SOF2 : FB_R_SOF1) & VRAM_MASK;
	fb_watch_addr_end = fb_watch_addr_start + fb_size;
}

void rend_cancel_emu_wait()
{
	if (config::ThreadedRendering)
	{
		FinishRender(NULL);
		renderEnd.Set();
		rend_allow_rollback();
		pvrQueue.cancelEnqueue();
		// Needed for android where this function may be called
		// from a thread different from the UI one
		pvrQueue.enqueue(PvrMessageQueue::Stop);
	}
}

void rend_set_fb_write_addr(u32 fb_w_sof1)
{
	if (fb_w_sof1 & 0x1000000)
		// render to texture
		return;
	fb_w_cur = fb_w_sof1;
}

void rend_swap_frame(u32 fb_r_sof)
{
	if (!config::EmulateFramebuffer && fb_r_sof == fb_w_cur && rend_is_enabled())
		pvrQueue.enqueue(PvrMessageQueue::Present);
}

void rend_disable_rollback()
{
	vramRollback.Reset();
}

void rend_allow_rollback()
{
	vramRollback.Set();
}

void rend_start_rollback()
{
	if (config::ThreadedRendering)
		vramRollback.Wait();
}

// Resync renderer-thread sync primitives after a rollback-ring rewind.
//
// Why: after `dc_deserialize` restores a mid-frame anchor where a render
// was in progress (pend_rend=true) AND the saved scheduler still has
// `render_end_schid` queued, the next SH4 dispatch will fire
// `rend_end_render()` which checks `if (pend_rend && ThreadedRendering)
// renderEnd.Wait()` (line ~561). The renderEnd cResetEvent is auto-reset
// and was already consumed by the LIVE forward path before rewind, so the
// Wait blocks forever — there's no rolled-back render in flight to set it.
//
// Fix: clear the sync state so the post-rewind SH4 doesn't wait on events
// the renderer can't fulfill. We do this once per rewind, after deserialize.
//
// The expected behavior from MVC2's perspective: SH4 thinks a render is
// in progress (queued at anchor), `rend_end_render` fires → sees
// pend_rend=false → falls through to the asic interrupt path. Game logic
// keeps making forward progress; the visible frame may differ from the
// live forward path's frame for ~1 frame, but that's acceptable for the
// rollback round-trip determinism test (we compare dc_serialize blobs,
// not framebuffer pixels).
void rend_resync_after_rollback()
{
	pend_rend = false;
	renderEnd.Set();
	vramRollback.Set();
	// Also clear ta_ctx's rqueue/frame_finished so QueueRender doesn't
	// block on a stale rqueue (the live forward path may have queued a
	// render that the renderer thread never had a chance to drain — or
	// would have drained, but headless+NO_REND has no real render thread).
	// FinishRender(DequeueRender()) handles both: clears rqueue to null and
	// Sets frame_finished. If rqueue was already null, FinishRender(nullptr)
	// just Sets frame_finished — harmless.
	FinishRender(DequeueRender());
	// Drain pvrQueue's non-Render messages and Set dequeueEvent so any
	// SH4 thread that's blocked in pvrQueue::enqueue() on a duplicate
	// Render message wakes up. Live-forward path may have left stale
	// Render entries in the queue that the post-rewind SH4 will see as
	// "duplicate" → would block on dequeueEvent forever.
	pvrQueue.cancelEnqueue();
}

void rend_enable_renderer(bool enabled) {
	rendererEnabled = enabled;
}

bool rend_is_enabled() {
	return rendererEnabled;
}

void rend_serialize(Serializer& ser)
{
	ser << fb_w_cur;
	ser << render_called;
	ser << fb_dirty;
	ser << fb_watch_addr_start;
	ser << fb_watch_addr_end;
	// V59: renderer-side state that affects deterministic replay.
	// docs/DC-SERIALIZE-AUDIT.md §2.2 + §3.7 — these were previously
	// reset to sentinels on every load, causing first-post-load frame
	// divergence (clearFramebuffer flag + tactx_Find LRU eviction).
	ser << fbAddrHistory;     // 2× u32
	ser << pend_rend;         // bool
	ser << rendererEnabled;   // bool
	ser << FrameCount;        // u32
}
void rend_deserialize(Deserializer& deser)
{
	deser >> fb_w_cur;
	if (deser.version() >= Deserializer::V20)
	{
		deser >> render_called;
		deser >> fb_dirty;
		deser >> fb_watch_addr_start;
		deser >> fb_watch_addr_end;
	}
	if (deser.version() >= Deserializer::V59)
	{
		deser >> fbAddrHistory;
		deser >> pend_rend;
		deser >> rendererEnabled;
		deser >> FrameCount;
	}
	else
	{
		// V<59 fallback: previous behaviour reset these to sentinels.
		// Acceptable for one-shot loads of older states (.state files
		// on disk); rollback ring depends on V59+ for byte-equality.
		pend_rend = false;
		fbAddrHistory[0] = 1;
		fbAddrHistory[1] = 1;
	}
}
