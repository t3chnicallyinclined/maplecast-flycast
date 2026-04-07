# WORKSTREAM: HEADLESS GPU-LESS FLYCAST SERVER

> **One-shot implementation guide.** Read top-to-bottom, execute in order. Every file path, line number, function, and gotcha is captured here so an agent can land a working headless `flycast` binary on a CPU-only Linux VPS in a single pass.

---

## 0. Context You Need Before Touching Code

### What this is
A new `--headless` mode for the existing flycast server binary that runs MVC2 + TA Mirror streaming **without an SDL window, without OpenGL, without a display, without a GPU.** The same `MAPLECAST_MIRROR_SERVER=1` pipeline that today runs on the home box (RTX 3090) becomes runnable on a $5/mo VPS, a Raspberry Pi, an LXC container with no `/dev/dri`, or a CI runner.

### Why this is possible at all (read this first)
The TA Mirror wire format is generated **entirely from CPU-side state**. The chain is:

1. **SH4** (CPU emulation) writes TA command lists into VRAM and triggers `STARTRENDER`. SH4 is a JIT/interpreter on the host CPU. No GPU. See [core/hw/sh4/](../core/hw/sh4/).
2. **`rend_start_render()`** in [core/hw/pvr/Renderer_if.cpp:377](../core/hw/pvr/Renderer_if.cpp#L377) builds a `TA_context` and enqueues a `Render` message into `pvrQueue`.
3. The render thread pops the message and calls `render()` at [Renderer_if.cpp:173](../core/hw/pvr/Renderer_if.cpp#L173). **Critically**, line 197-198 calls `maplecast_mirror::serverPublish(taContext)` **BEFORE** `renderer->Process(taContext)`. The mirror captures the TA bytes from RAM directly — no rasterization required.
4. `renderer->Process()` runs `ta_parse(ctx, true)` to populate `rend_context`. **This is CPU-only.** It is required because the mirror's PVR snapshot pulls a few derived fields, and because `present()` later signals SH4 stop (frame cadence).
5. `renderer->Render()` is the only step that actually touches the GPU (rasterizes `rend_context` to a framebuffer via GLES/Vulkan/DX). **The mirror does not depend on this output.**
6. `renderer->Present()` sets `presented = true` and calls `emu.getSh4Executor()->Stop()` ([Renderer_if.cpp:260-261](../core/hw/pvr/Renderer_if.cpp#L260)). This is the per-frame heartbeat that lets `rend_single_frame` return true. **It does not require a GPU; the base class default at [Renderer_if.h:81](../core/hw/pvr/Renderer_if.h#L81) is `virtual bool Present() { return true; }`.**

So the *only* GPU-dependent step in the whole loop is `renderer->Render()`. Everything upstream — SH4, SPG, TA capture, VRAM diff, PVR snapshot, zstd, WebSocket broadcast — is pure CPU. **The work in this workstream is to make `renderer->Render()` a no-op and prevent the boot path from ever creating a GL context.**

### The renderer that already does this
[core/rend/norend/norend.cpp](../core/rend/norend/norend.cpp) is **30 lines** and is already in the build. It implements the `Renderer` interface as:

```cpp
struct norend : Renderer {
    bool Init() override { return true; }
    void Term() override { }
    void Process(TA_context* ctx) override {
        rendContext = &ctx->rend;
        ta_parse(ctx, true);          // ← exactly what mirror needs
    }
    bool Render() override { return !rendContext->isRTT; }
    void RenderFramebuffer(const FramebufferInfo& info) override { rendContext = nullptr; }
    rend_context *rendContext;
};
```

This is the renderer the headless mode will use. It has no GL calls, no shader compilation, no texture uploads. `ta_parse` runs on CPU. `Render()` returns true (signaling "this was a screen render, not RTT") so `present()` runs, which signals frame cadence. **You are not writing a new renderer — you are wiring the existing `norend` into the boot path.**

### What is NOT in scope
- **No new wire format.** The on-wire bytes are byte-identical to today (commit `466d72d54` determinism guarantee). Verified by the existing `MAPLECAST_DUMP_TA=1` rig.
- **No changes to the VPS relay.** It already speaks the wire. If you change the wire, you have a regression — fix it.
- **No changes to the wasm renderer or `king.html`.** Browsers continue to consume the same stream.
- **No changes to NOBD input, gamestate reading, or telemetry.** All CPU-side, all already work.
- **No removal of the H.264/NVENC path.** Just don't compile it on the headless box (`MAPLECAST_NVENC=0`, `MAPLECAST_CUDA=0`).
- **No new threading.** The existing `pvrQueue` + render thread + WS thread design is fine; with norend the render thread just does less work.

### What this unlocks (the why)
- Move the emulator next to the relay on the VPS — kill the home→VPS internet hop entirely (~10–40ms saved for remote spectators).
- Run multiple game instances on a single CPU box for tournaments, side rooms, training sessions.
- Spin up ephemeral instances in CI for the determinism test rig (no GPU runners required).
- Cheap dev environment: edit + build + run on a laptop with no discrete GPU.
- Future N-cabinet horizontal scaling without N GPUs.

### What this does NOT unlock
- **Local sub-millisecond play.** NOBD sticks at the home cab still want to talk to a flycast on the LAN ([ARCHITECTURE.md:943-950](ARCHITECTURE.md#L943-L950)). VPS-headless adds your home↔VPS RTT (~20–40ms typical) to input latency. Headless is for **spectator-first / remote-tournament** deployments. The home cab keeps its 3090-backed flycast for local play. The two are not mutually exclusive — they both consume/produce the same wire.

---

## 1. Success Criteria — How You Know You're Done

Phase-gated. No "mostly works".

| Gate | Acceptance test |
|------|-----------------|
| **G1** | `flycast` builds on Linux x86_64 with `-DUSE_OPENGL=OFF -DUSE_VULKAN=OFF -DUSE_SDL=OFF -DMAPLECAST_HEADLESS=ON`. Resulting binary has zero references to `libGL`, `libEGL`, `libSDL2`, `libGLX`. Verified with `ldd build/flycast | grep -iE 'gl\|sdl\|x11'` returning empty. |
| **G2** | `MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1 MAPLECAST_HEADLESS=1 ./build/flycast mvc2.chd` boots MVC2 to the title screen on a box with `DISPLAY=` unset and no `/dev/dri`. Process stays alive, RSS < 500MB, CPU usage steady ~80–120% on one core. |
| **G3** | The headless process serves WebSocket frames on `:7200`. `websocat -b ws://localhost:7200 \| head -c 8 \| xxd` shows `ZCST` magic. |
| **G4** | The determinism test rig (`MAPLECAST_DUMP_TA=1`) run against a headless server and a normal flycast client (`MAPLECAST_MIRROR_CLIENT=1`) shows **TA byte: 5395 match, 0 differ** over a 90-second run that includes a scene transition (title → character select → match). Same recipe as [ARCHITECTURE.md:347-365](ARCHITECTURE.md#L347-L365). |
| **G5** | A browser at `https://nobd.net` connects to a relay whose upstream is a headless flycast and renders MVC2 visually identical to the same browser run against the GPU-backed home flycast. Compare side-by-side for ≥60s including a scene transition. Pixel-perfect, same hashes. |
| **G6** | NOBD stick UDP input → headless flycast → kcode[] → CMD9 → game responds. Press a button on a stick pointed at `MAPLECAST_PORT=7100`, see the character react in the WASM client. |
| **G7** | A 2-vCPU VPS (any cloud provider, no GPU SKU) sustains 60.0fps for 30 minutes with one MVC2 instance, ≤1 dropped frame, ≤880µs publish time per frame ([ARCHITECTURE.md:557-565](ARCHITECTURE.md#L557-L565) budget). Telemetry verified via `/metrics`. |
| **G8** | Reboot/crash recovery: `systemctl restart maplecast-headless` brings the instance back, browsers reconnect, gameplay resumes from the next SYNC, no manual steps. |
| **G9** | Documented Dockerfile builds the headless binary in a clean `debian:12-slim` image and produces a `< 200MB` final image with no GPU runtime libs. `docker run` to G2-G7 verified inside the container. |
| **G10** | `docs/ARCHITECTURE.md` and `docs/VPS-SETUP.md` updated with the headless mode, env vars, and deployment recipe. The dead `MAPLECAST_NVENC` path is documented as "GPU-only, not built in headless." |

**Until G10 is green, this workstream is not done.** Don't open a PR for "G1-G4 works". Ship the whole thing.

---

## 2. Prerequisites — What Must Already Exist

Verify before starting Phase 1. If any is false, fix that first.

- [ ] Current `master`/`maplecast` branch builds and runs `MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1 ./build/flycast mvc2.chd` on the home box, serves frames on `:7200`, and the wasm browser at `https://nobd.net` renders correctly. **If this baseline is broken, fix it before starting headless work — you cannot regress something that doesn't currently work.**
- [ ] The determinism test rig from [ARCHITECTURE.md:336-365](ARCHITECTURE.md#L336-L365) is green. `MAPLECAST_DUMP_TA=1` server + client → 0 byte differences over a multi-scene run. This is your regression baseline — every change in this workstream must keep it green.
- [ ] The mirror server's `serverPublish()` is being called before `renderer->Process()` at [Renderer_if.cpp:197-198](../core/hw/pvr/Renderer_if.cpp#L197). If anyone has reordered this, stop and fix it — the entire headless mode rests on this ordering.
- [ ] [core/rend/norend/norend.cpp](../core/rend/norend/norend.cpp) exists and has not been deleted. The `rend_norend()` factory function is declared at [Renderer_if.cpp:290](../core/hw/pvr/Renderer_if.cpp#L290).
- [ ] Recent commits `466d72d54` (deterministic wire) and `413e4931e` (scene-change garble fixes) are in your branch. The eight bugs in [ARCHITECTURE.md:433-493](ARCHITECTURE.md#L433-L493) are fixed.

---

## 3. Expert Consultations — Whose Problems You're Inheriting

### The Renderer Pipeline Expert
**Who:** [core/hw/pvr/Renderer_if.cpp](../core/hw/pvr/Renderer_if.cpp) — `PvrMessageQueue::render()` and friends.

**What they know:**
- The render thread is created by the GUI/main loop, NOT by the renderer. The renderer is just an object plugged into a queue.
- `pvrQueue.enqueue(Render)` and `pvrQueue.enqueue(Present)` are the two messages that drive frame cadence. `Render` runs `Process` then `Render`. `Present` runs `Present` (which sets `presented = true` and stops SH4 in non-threaded mode). With norend, both are valid no-cost ops.
- `rend_start_render()` at line 377 unconditionally calls `palette_update()` and `pvrQueue.enqueue(Render)`. Mirror server depends on this firing once per `STARTRENDER`. Don't gate it.
- `rend_vblank()` at line 470 is called from the SPG scheduler thread (CPU). This is what drives `maple_DoDma()` → CMD9 → input read. **It runs whether or not there's a renderer.** Headless does not affect input.

**Warnings:**
- `rend_init_renderer()` at line 340 has a fallback: if the configured renderer's `Init()` fails, it deletes the failed renderer and creates a `norend`. **But it returns `success` (false) anyway**, which causes [mainui.cpp:106-109](../core/ui/mainui.cpp#L106) to pop an error dialog. Do not rely on this fallback for headless — wire `norend` in cleanly via the explicit path so `rend_init_renderer()` returns `true`.
- The `rendererEnabled` global is toggled by `rend_enable_renderer()`. Don't conflate "headless" with "renderer disabled" — when `rendererEnabled = false`, `rend_vblank` skips the framebuffer-direct path, which can break some games that don't use TA at all. Headless wants `rendererEnabled = true` with a no-op renderer, NOT `rendererEnabled = false`.
- `config::ThreadedRendering` matters: when true, `Render` and `Present` run on a separate thread; when false, they run inline and `present()` calls `emu.getSh4Executor()->Stop()` to throttle. Headless works in both modes but **inline mode has lower jitter** and is recommended for VPS deployment. Default to non-threaded for headless.

### The Boot/Window Expert
**Who:** [core/nullDC.cpp](../core/nullDC.cpp), [core/oslib/oslib.cpp](../core/oslib/oslib.cpp), [core/sdl/sdl.cpp](../core/sdl/sdl.cpp), [core/ui/mainui.cpp](../core/ui/mainui.cpp).

**What they know:**
- `flycast_init()` at [nullDC.cpp:75](../core/nullDC.cpp#L75) calls `gui_init()` then `os_CreateWindow()` then `os_SetupInput()`. `os_CreateWindow` dispatches to `sdl_window_create()` at [oslib.cpp:430](../core/oslib/oslib.cpp#L430). This is where the SDL window + GL context come into existence.
- `imguiDriver` is set globally inside the GL context creation path ([gl_context.cpp:80](../core/wsi/gl_context.cpp#L80)). It is what `mainui_loop` uses to call `imguiDriver->present()` once per UI frame.
- `mainui_loop` at [mainui.cpp:118](../core/ui/mainui.cpp#L118) hard-requires `imguiDriver != nullptr` ([line 138](../core/ui/mainui.cpp#L138)). If null, it sets `forceReinit = true` and tries to switch render APIs. **This is a busy-loop trap for headless** — there's no GL to switch to. Bypass it.
- `mainui_rend_frame()` at [mainui.cpp:40](../core/ui/mainui.cpp#L40) calls `os_DoEvents()` (SDL event pump) and `os_UpdateInputState()` ([oslib.cpp:483](../core/oslib/oslib.cpp#L483)). Both are guarded by `#if defined(USE_SDL)` so they compile to no-ops without SDL — but only if you actually compile WITHOUT SDL. The runtime env-var headless mode must still tolerate the SDL versions doing nothing useful (they will, when there's no window).
- The mirror client mode at [mainui.cpp:66-78](../core/ui/mainui.cpp#L66) runs without `gui_open` and calls `renderer->Render()` / `renderer->Present()` directly. **This is the closest existing analogue to what headless server mode should look like** — copy its loop structure for the headless server branch.

**Warnings:**
- `gui_init()` initializes ImGui state and fonts. It doesn't create a GL context by itself, but `imguiDriver` is created later by the GL/Vulkan/DX context. Calling `gui_init()` headlessly is fine; calling `gui_display_ui()` headlessly is NOT — it will dereference `imguiDriver`.
- `os_SetupInput()` enumerates SDL/evdev gamepads. Safe to skip in headless — input arrives via `MAPLECAST_PORT=7100` UDP, not the local input subsystem.
- `lua::init()` at [nullDC.cpp:94](../core/nullDC.cpp#L94) is fine headless; Lua scripts can still run.

### The Mirror Server Expert
**Who:** [core/network/maplecast_mirror.cpp](../core/network/maplecast_mirror.cpp), [maplecast_ws_server.cpp](../core/network/maplecast_ws_server.cpp), [maplecast_compress.h](../core/network/maplecast_compress.h).

**What they know:**
- `serverPublish()` reads `vram[]`, `pvr_regs[]`, `taContext->rend.*`, and a few hardware register snapshots. **Zero GL calls.** Verified by grep — the only `#include` from `rend/` is `TexCache.h` for `VramLockedWriteOffset()` (mprotect, not GL).
- `VramLockedWriteOffset(pageOff)` at [maplecast_mirror.cpp:1251,1420](../core/network/maplecast_mirror.cpp#L1251) is a CPU-side `mprotect` to clear texture-cache mark bits. It works headless. The texture cache itself is GL-side state (in the GL renderer); norend has no texture cache, so the call is harmless and idempotent.
- `memwatch::unprotect()` at [maplecast_mirror.cpp:348,387,576,731](../core/network/maplecast_mirror.cpp#L348) is plain `mprotect(PROT_READ|WRITE)`. Works headless.
- The eight wire-format bugs in [ARCHITECTURE.md:433-493](ARCHITECTURE.md#L433-L493) are all in the producer/decoder paths and have nothing to do with rendering. They stay fixed in headless mode automatically.

**Warnings:**
- `serverPublish()` reads `_pvrAtomicSnap` after taking a thread-local snapshot of `pvr_regs[]` at the top of the function ([ARCHITECTURE.md:320-327](ARCHITECTURE.md#L320-L327)). This is safe because the SPG scheduler thread is the only writer and the snapshot is atomic. Headless does not affect this — the SPG scheduler runs on its own CPU thread regardless.
- The compressor `_compressor` is pre-allocated at server init. Don't move its lifetime around when refactoring boot for headless.
- Texture-cache reset flags (`renderer->resetTextureCache = true` etc) in [maplecast_mirror.cpp](../core/network/maplecast_mirror.cpp) write to `renderer->`. With norend, these writes go to a `Renderer*` whose `resetTextureCache` field still exists (it's on the base class) but is never read. Harmless. Do NOT add an `if (renderer && renderer->hasTextureCache)` guard — it's a needless complication.

### The Build System Expert
**Who:** [CMakeLists.txt](../CMakeLists.txt) — top-level Flycast CMake.

**What they know:**
- `USE_OPENGL` is `ON` by default ([CMakeLists.txt:90](../CMakeLists.txt#L90)) and pulls in `libGL`, libretro GLSM, all renderer backends.
- `USE_SDL` is implicit (no top-level option, set inline at [line 542](../CMakeLists.txt#L542)) and pulls in `libSDL2`.
- There is no existing `MAPLECAST_HEADLESS` or `NO_REND` option at top level. `NO_REND` is referenced by [Renderer_if.cpp:299](../core/hw/pvr/Renderer_if.cpp#L299) and forces only norend in `rend_create_renderer()` — but no CMake option defines it today. **This is your hook.** Add `MAPLECAST_HEADLESS` as a top-level CMake option that sets `NO_REND` and disables `USE_OPENGL`/`USE_VULKAN`/`USE_SDL`.

**Warnings:**
- Some sources unconditionally `#include` GLES/GL headers (`core/rend/gles/`, `core/rend/gl4/`). With `NO_REND` they shouldn't be in the build. Verify by listing what `glob(... rend/gles/*.cpp)` matches and confirming they're inside an `if(USE_OPENGL)` block.
- `imgui_driver` interface is a header-only abstract base. ImGui itself can be compiled without any backend, but the driver implementations live with their renderer (`OpenGLDriver` in `wsi/gl_context.cpp`, `VulkanDriver`, etc.). For headless, you DO NOT compile any of these. `imguiDriver` stays `nullptr`.
- The libretro target (`flycast_libretro`) is a separate CMake target that runs through Emscripten for the wasm build. **Headless is NOT a libretro build.** It's a normal native build with NO_REND. Don't conflate the two.

---

## 4. Architecture — The Boot/Frame Diagram

```
┌────────────────────────────────────────────────────────────────────┐
│  HEADLESS FLYCAST BOOT (env: MAPLECAST_HEADLESS=1)                 │
└────────────────────────────────────────────────────────────────────┘

main()
  └─ flycast_init()                              [core/nullDC.cpp:54]
        ├─ config::open()
        ├─ LogManager::Init()
        ├─ gui_init()                             ← OK headless
        │
        ├─ if (headless) SKIP                     ← NEW gate
        │  else: os_CreateWindow()                [oslib.cpp:430]
        │           └─ sdl_window_create()        [sdl.cpp:825]
        │                ├─ SDL_CreateWindow
        │                ├─ create GL context
        │                └─ imguiDriver = new OpenGLDriver()
        │
        ├─ if (headless) SKIP                     ← NEW gate
        │  else: os_SetupInput()                  [oslib.cpp:448]
        │
        ├─ debugger::init() (if GDB)
        └─ lua::init()

main() continues
  └─ mainui_loop()                                [core/ui/mainui.cpp:118]
        ├─ mainui_init()
        │     └─ rend_init_renderer()             [Renderer_if.cpp:340]
        │           ├─ rend_create_renderer()
        │           │   #ifdef NO_REND
        │           │       renderer = rend_norend()    ← THE KEY LINE
        │           │   #else
        │           │       switch(config::RendererType) → GLES2/GL4/VK/DX
        │           │   #endif
        │           └─ renderer->Init()           ← norend::Init returns true
        │
        └─ while (mainui_enabled):
              ├─ mainui_rend_frame()              [mainui.cpp:40]
              │     ├─ os_DoEvents() — no-op without SDL
              │     ├─ os_UpdateInputState() — no-op without SDL
              │     ├─ if (headless && server) {
              │     │       emu.render()           ← runs SH4 to next vblank,
              │     │                                triggers rend_start_render
              │     │                                triggers serverPublish()
              │     │                                triggers norend::Process
              │     │                                triggers norend::Render (no-op)
              │     │                                triggers Present (cadence stop)
              │     │   }
              │     └─ return true
              │
              ├─ if (imguiDriver) imguiDriver->present()
              │  // headless: imguiDriver is nullptr → skip cleanly,
              │  // do NOT set forceReinit
              │
              └─ // headless: no render-API switching, no forceReinit branch


┌────────────────────────────────────────────────────────────────────┐
│  HEADLESS PER-FRAME (CPU only — NO GPU CALLS)                      │
└────────────────────────────────────────────────────────────────────┘

emu.render()                                    ~16.67ms wall-clock
  │
  └─ SH4 dynarec runs ~200K instructions until next vblank
        │
        ├─ Game code writes TA command list to VRAM via store32_async
        │
        ├─ Game writes STARTRENDER register
        │   └─ rend_start_render()              [Renderer_if.cpp:377]
        │       ├─ tactx_Pop() → TA_context*
        │       ├─ FillBGP(ctx)
        │       ├─ palette_update()             ← CPU palette transform
        │       └─ pvrQueue.enqueue(Render)
        │           └─ (non-threaded) execute(Render) inline:
        │               │
        │               ├─ maplecast_mirror::serverPublish(taContext)  ← THE WIRE
        │               │   ├─ Snapshot pvr_regs into _pvrAtomicSnap
        │               │   ├─ Diff VRAM pages (memcmp 2048×4KB)
        │               │   ├─ TA delta encode
        │               │   ├─ Assemble frame envelope
        │               │   ├─ MirrorCompressor.compress (zstd L1)
        │               │   └─ broadcastBinary() → WS:7200
        │               │
        │               ├─ renderer->Process(ctx)  ← norend::Process
        │               │   └─ ta_parse(ctx, true) — CPU only, populates rend_context
        │               │
        │               ├─ renderer->Render()      ← norend::Render returns true (no GL)
        │               │
        │               └─ renderer->Present()     ← base default returns true
        │                   └─ presented = true
        │                   └─ emu.getSh4Executor()->Stop()  ← frame heartbeat
        │
        └─ SH4 sees Stop, returns from emu.render()

Mirror clients (browser, native client, relay) consume :7200 unchanged.
```

**Key invariant:** with norend wired in, `serverPublish()` still runs at exactly the same point in the frame as before, with exactly the same inputs (vram, pvr_regs, taContext). The wire bytes are guaranteed identical to the GPU-backed path. **This is enforced by the determinism rig in G4.**

---

## 5. Implementation Phases

Execute in order. Do not skip phases. After each phase, run the listed verification before proceeding.

### Phase 1 — Add `MAPLECAST_HEADLESS` env var + boot gates

**Goal:** boot the existing GPU-backed flycast with `MAPLECAST_HEADLESS=1` and have it skip window/input setup at runtime, while still wiring norend at the renderer level. **No CMake changes yet.** This proves the runtime path works before you tear out the build deps.

**Files to edit:**

1. **[core/network/maplecast_mirror.h](../core/network/maplecast_mirror.h)** — add a new accessor:
   ```cpp
   namespace maplecast_mirror {
       bool isHeadless();   // true iff MAPLECAST_HEADLESS=1 in env at startup
   }
   ```

2. **[core/network/maplecast_mirror.cpp](../core/network/maplecast_mirror.cpp)** — implement once at module init:
   ```cpp
   static bool _headless = false;
   bool isHeadless() { return _headless; }

   // In the existing init function (initServer or similar), add:
   _headless = std::getenv("MAPLECAST_HEADLESS") != nullptr;
   if (_headless) NOTICE_LOG(NETWORK, "MapleCast: HEADLESS mode (no GPU/window)");
   ```

3. **[core/nullDC.cpp:89-90](../core/nullDC.cpp#L89)** — gate `os_CreateWindow()` and `os_SetupInput()`:
   ```cpp
   gui_init();
   if (!std::getenv("MAPLECAST_HEADLESS")) {
       os_CreateWindow();
       os_SetupInput();
   } else {
       NOTICE_LOG(BOOT, "MAPLECAST_HEADLESS=1 — skipping window + input setup");
   }
   ```
   Read the env var directly here (not via `maplecast_mirror::isHeadless()`) because nullDC.cpp boots before maplecast_mirror init.

4. **[core/hw/pvr/Renderer_if.cpp:297-338](../core/hw/pvr/Renderer_if.cpp#L297) `rend_create_renderer()`** — add an explicit headless branch BEFORE the switch:
   ```cpp
   static void rend_create_renderer()
   {
       if (std::getenv("MAPLECAST_HEADLESS")) {
           renderer = rend_norend();
           return;
       }
   #ifdef NO_REND
       renderer = rend_norend();
   #else
       switch (config::RendererType) {
           // ...existing switch unchanged...
       }
   #endif
   }
   ```
   This ensures `rend_init_renderer()` returns `true` cleanly (norend->Init() returns true), so [mainui.cpp:106-109](../core/ui/mainui.cpp#L106) does not pop a `gui_error()` dialog.

5. **[core/ui/mainui.cpp:138-139](../core/ui/mainui.cpp#L138)** — bypass the `imguiDriver == nullptr → forceReinit` trap when headless:
   ```cpp
   if (imguiDriver == nullptr && !std::getenv("MAPLECAST_HEADLESS"))
       forceReinit = true;
   ```
   And gate the `imguiDriver->present()` call ([mainui.cpp:133](../core/ui/mainui.cpp#L133)):
   ```cpp
   if (mainui_rend_frame() && imguiDriver != nullptr) {
       try { imguiDriver->present(); } catch (...) { forceReinit = true; }
   }
   ```
   (If `imguiDriver` is null and headless, just skip — the frame still ran via `emu.render()`.)

6. **[core/ui/mainui.cpp:141-169](../core/ui/mainui.cpp#L141)** — gate the render-API switching block:
   ```cpp
   if (!std::getenv("MAPLECAST_HEADLESS") &&
       (config::RendererType != currentRenderer || forceReinit))
   {
       // ...existing switchRenderApi block unchanged...
   }
   ```
   Headless never switches APIs.

**Verification (Phase 1 exit):**
- Build with the existing CMake flags (no changes), GPU still wired in.
- Run with `DISPLAY=` set (so GL still works) and `MAPLECAST_HEADLESS=1` to confirm the gates fire correctly without breaking the GPU path.
- Then run with `unset DISPLAY` and `MAPLECAST_HEADLESS=1`. Process must boot, serve `:7200`, and stay alive for ≥60s. `pgrep flycast` returns a PID. CPU usage ~80–120% on one core.
- Run determinism rig (`MAPLECAST_DUMP_TA=1`) headless server vs. GPU client → 0 byte differences over a multi-scene run. **This is the gate. If it fails, fix before Phase 2.**

### Phase 2 — Mirror server frame loop without `imguiDriver`

**Goal:** make the headless main loop call `emu.render()` cleanly without hitting any code path that touches GL or assumes a window exists.

**Background:** in the existing flow, `mainui_rend_frame()` calls `emu.render()` which internally drives SH4 → vblank → render-thread enqueue. The render thread (or inline path) calls into the renderer. With norend wired in (Phase 1), the renderer's calls are no-ops, but `emu.render()` itself may have other GL-touching paths.

**Files to edit / verify:**

1. **`emu.render()`** — find this in `core/emulator.cpp` (or wherever it lives now). Read it. Identify any direct GL/SDL calls. Most likely there are none — it just runs the SH4 executor and returns when SH4 stops. If you find any, gate them on `!isHeadless()`.

2. **[core/ui/gui.cpp](../core/ui/gui.cpp)** — `gui_init()` and `gui_term()` must be safe to call without a GL context. Verify they don't crash. They should be fine because ImGui state is independent of any backend; the backend (`imguiDriver`) is only touched by `gui_display_ui()` and `gui_endFrame()`, neither of which we call in headless.

3. **[core/oslib/oslib.cpp:430-446](../core/oslib/oslib.cpp#L430)** — `os_DestroyWindow()` is called from `flycast_term()` ([nullDC.cpp:155](../core/nullDC.cpp#L155)). Gate it the same way:
   ```cpp
   void flycast_term() {
       gui_cancel_load();
       lua::term();
       emu.term();
       if (!std::getenv("MAPLECAST_HEADLESS"))
           os_DestroyWindow();
       gui_term();
       os_TermInput();
   }
   ```

4. **[core/ui/mainui.cpp:40-102](../core/ui/mainui.cpp#L40) `mainui_rend_frame()`** — verify the `else` branch at line 80 (the normal `emu.render()` path) is what runs in headless server mode. It should: `gui_is_open()` is false at startup (no GUI), `maplecast_mirror::isClient()` is false (we're a server), so the third branch fires and `emu.render()` runs. Confirm by adding a one-shot DEBUG_LOG.

**Verification (Phase 2 exit):**
- Headless run sustains 60.0fps for 5 minutes. `fps` field in `/metrics` confirms.
- No `[BOOT]` or `[RENDERER]` errors in the log.
- The `imguiDriver == nullptr` warning that Phase 1 might still emit on the first frame is gone.
- Mirror client connecting to the headless server renders identical frames (visually verified side-by-side with a GPU-backed run).

### Phase 3 — CMake `MAPLECAST_HEADLESS` build option (compile-out GL/SDL)

**Goal:** produce a binary that has zero linkage to `libGL`, `libEGL`, `libSDL2`, `libGLX`. This is what makes the binary deployable on a no-GPU VPS without dragging the GPU runtime in.

**Files to edit:**

1. **[CMakeLists.txt](../CMakeLists.txt)** — add option near line 90:
   ```cmake
   option(MAPLECAST_HEADLESS "Build a headless flycast server (no GL, no SDL, no window)" OFF)

   if(MAPLECAST_HEADLESS)
       set(USE_OPENGL OFF CACHE BOOL "" FORCE)
       set(USE_VULKAN OFF CACHE BOOL "" FORCE)
       set(USE_SDL OFF CACHE BOOL "" FORCE)   # if there's an option; otherwise gate the inline target_compile_definitions
       set(USE_DX9 OFF CACHE BOOL "" FORCE)
       set(USE_DX11 OFF CACHE BOOL "" FORCE)
       add_compile_definitions(NO_REND MAPLECAST_HEADLESS)
       message(STATUS "MapleCast: HEADLESS build (no GPU/window deps)")
   endif()
   ```

2. **[CMakeLists.txt:542](../CMakeLists.txt#L542)** — gate the SDL `target_compile_definitions`:
   ```cmake
   if(NOT MAPLECAST_HEADLESS)
       target_compile_definitions(${PROJECT_NAME} PRIVATE USE_SDL USE_SDL_AUDIO)
       # ...existing SDL link/include...
   endif()
   ```

3. **Audit all `if(USE_OPENGL)`, `if(USE_VULKAN)`, `if(USE_DX*)` blocks** — confirm they cleanly exclude their sources, link libs, and include dirs when the option is OFF. Specifically check:
   - `core/wsi/gl_context.cpp` — must NOT compile in headless (it defines `OpenGLDriver`).
   - `core/rend/gles/`, `core/rend/gl4/` — entire directories must be excluded.
   - `core/rend/vulkan/`, `core/rend/dx9/`, `core/rend/dx11/` — same.
   - `core/sdl/sdl.cpp` — must NOT compile in headless. The function declarations in `core/sdl/sdl.h` will leave linker errors if anything still references `sdl_window_create` etc; those references all live in `oslib.cpp` inside `#if defined(USE_SDL)` blocks, so they should also drop out cleanly.

4. **`core/rend/norend/norend.cpp`** must remain in the build unconditionally. Verify the CMake glob includes it.

5. **Audio:** SDL also provides audio. Headless servers don't need audio output. Confirm there's a `nullaud` or that the audio init no-ops cleanly without `USE_SDL_AUDIO`. If not, add a `null_audio.cpp` shim that satisfies the audio interface. The mirror stream does not include audio (TA wire is video-only), so this is purely about not crashing on init.

6. **Stub any leftover symbols.** Build will fail the first 1-3 times with "undefined reference to `os_CreateWindow`" or similar. Trace each one to its caller:
   - If the caller is gated by `USE_SDL`, the gate is wrong — fix it.
   - If the caller is unconditional, add `MAPLECAST_HEADLESS` gate.
   - **Do NOT introduce dummy stubs that compile-and-link but would crash if called.** Either gate the caller or leave a hard `die("not headless")` so misuse is loud.

**Verification (Phase 3 exit):**
- `mkdir build-headless && cd build-headless && cmake -DMAPLECAST_HEADLESS=ON -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)` succeeds.
- `ldd build-headless/flycast | grep -iE 'libGL|libEGL|libSDL|libX11|libGLX|libdrm'` returns **empty**.
- `nm build-headless/flycast | grep -iE 'sdl_window_create|os_CreateWindow' | grep -v ' U '` returns **empty** (no defined symbols, only optionally an unresolved one — which itself should be empty too).
- Binary size < 60MB.
- Run `MAPLECAST=1 MAPLECAST_MIRROR_SERVER=1 ./flycast mvc2.chd` — note that `MAPLECAST_HEADLESS` env var is no longer needed at runtime because the binary is always headless. Boot still works. G2-G6 still pass.
- Determinism rig still 0 differ.

### Phase 4 — Container + systemd deployment

**Goal:** ship-ready VPS deployment.

**Files to add:**

1. **`Dockerfile.headless`** at repo root:
   ```dockerfile
   # Build stage
   FROM debian:12-slim AS build
   RUN apt-get update && apt-get install -y \
       build-essential cmake git ninja-build pkg-config \
       libzip-dev libcurl4-openssl-dev libssl-dev zlib1g-dev \
       && rm -rf /var/lib/apt/lists/*
   WORKDIR /src
   COPY . .
   RUN cmake -B build-headless -GNinja \
         -DMAPLECAST_HEADLESS=ON \
         -DCMAKE_BUILD_TYPE=Release \
         -DUSE_OPENGL=OFF -DUSE_VULKAN=OFF \
       && cmake --build build-headless

   # Runtime stage
   FROM debian:12-slim
   RUN apt-get update && apt-get install -y \
       libcurl4 libssl3 libzip4 zlib1g ca-certificates \
       && rm -rf /var/lib/apt/lists/*
   COPY --from=build /src/build-headless/flycast /usr/local/bin/flycast
   COPY --from=build /src/web/bios/ /opt/maplecast/bios/
   # ROM and config mounted at runtime
   ENV MAPLECAST=1
   ENV MAPLECAST_MIRROR_SERVER=1
   ENV MAPLECAST_PORT=7100
   ENV MAPLECAST_STREAM_PORT=7200
   EXPOSE 7100/udp 7200/tcp
   ENTRYPOINT ["/usr/local/bin/flycast"]
   CMD ["/data/mvc2.chd"]
   ```
   Final image must be `< 200MB` and contain no GPU runtime libs.

2. **`deploy/systemd/maplecast-headless.service`**:
   ```ini
   [Unit]
   Description=MapleCast Headless Flycast Server
   After=network-online.target
   Wants=network-online.target

   [Service]
   Type=simple
   User=maplecast
   Group=maplecast
   WorkingDirectory=/opt/maplecast
   Environment=MAPLECAST=1
   Environment=MAPLECAST_MIRROR_SERVER=1
   Environment=MAPLECAST_PORT=7100
   Environment=MAPLECAST_STREAM_PORT=7200
   ExecStart=/usr/local/bin/flycast /opt/maplecast/roms/mvc2.chd
   Restart=always
   RestartSec=5
   # No GPU access, no display, no audio
   PrivateDevices=yes
   ProtectSystem=strict
   ReadWritePaths=/opt/maplecast/state /var/log/maplecast
   NoNewPrivileges=yes

   [Install]
   WantedBy=multi-user.target
   ```

3. **`deploy/headless-vps.sh`** — provisioning script (mirrors `relay/deploy.sh` style):
   ```bash
   #!/usr/bin/env bash
   # Usage: ./deploy/headless-vps.sh user@vps-ip
   set -euo pipefail
   HOST="$1"
   docker build -f Dockerfile.headless -t maplecast-headless:latest .
   docker save maplecast-headless:latest | ssh "$HOST" 'sudo docker load'
   scp deploy/systemd/maplecast-headless.service "$HOST":/tmp/
   ssh "$HOST" 'sudo install -m 0644 /tmp/maplecast-headless.service /etc/systemd/system/ \
                 && sudo systemctl daemon-reload \
                 && sudo systemctl enable --now maplecast-headless'
   ```

4. **Wire to relay:** the relay's upstream URL config (in `relay/src/main.rs` or its CLI args) must point to the headless instance instead of the home box. If running headless on the SAME VPS as the relay, set upstream to `ws://127.0.0.1:7200`. Document this in `docs/VPS-SETUP.md`.

**Verification (Phase 4 exit):**
- G7: 30-minute soak on a real 2-vCPU VPS, ≤1 dropped frame, ≤880µs publish time average.
- G8: `systemctl restart maplecast-headless` mid-match → browsers reconnect within 5s and resume rendering from the next SYNC. No stuck spectators.
- G9: Docker image `< 200MB`. `docker run --rm maplecast-headless:latest /data/mvc2.chd` (with ROM mounted) passes G2-G6 inside the container.

### Phase 5 — Documentation + cleanup

**Files to update:**

1. **[docs/ARCHITECTURE.md](ARCHITECTURE.md)** — add a new section after "Mode 2: H.264 (Legacy)":
   - **"Mode 3: Headless (No GPU)"**
   - Diagram of the headless boot path
   - The norend wiring and why it works (the `serverPublish()` ordering invariant)
   - The new env var `MAPLECAST_HEADLESS` (runtime gate, optional if built with the CMake flag)
   - Note that headless mode ships byte-identical wire to the GPU mode, enforced by the determinism rig
   - Explicit note: "headless does NOT remove the H.264/NVENC path from the codebase, it just doesn't compile it. To use H.264 streaming you still need a GPU build."

2. **[docs/VPS-SETUP.md](VPS-SETUP.md)** — add a section "Running flycast on the VPS (headless)":
   - Hardware requirements (2 vCPU, 1GB RAM, no GPU)
   - Build/deploy via Docker
   - systemd setup
   - How to point the relay at the local headless instance
   - Performance expectations and the publish-time budget

3. **`docs/WORKSTREAM-HEADLESS-SERVER.md`** (this file) — add a "STATUS: COMPLETE" header at the top once G1-G10 are all green. Link the PR.

4. **Memory:** the agent should add a memory entry in `/home/tris/.claude/projects/-home-tris-projects-maplecast-flycast/memory/` after completion:
   - Type: project
   - Name: "Headless Server Shipped"
   - Body: "Flycast can run on a CPU-only VPS via `-DMAPLECAST_HEADLESS=ON` build or `MAPLECAST_HEADLESS=1` env var. Wires norend renderer, skips SDL/window/GL. Wire-byte identical to GPU build (determinism rig green). VPS deployment lives in `deploy/`. **Why:** unlocks cheap multi-instance deployment and removes the home→VPS hop. **How to apply:** for any VPS-side flycast work, default to the headless path; only the home cab uses the GPU build."

---

## 6. Pitfalls & Tripwires (read before debugging)

These are the things that will burn you. Read each one before you start, and re-read whichever applies when you hit a wall.

### "It boots but no frames go out on :7200"
- `serverPublish()` is gated on `maplecast_mirror::isServer()`. Confirm `MAPLECAST_MIRROR_SERVER=1` is in the env.
- `rend_start_render()` only fires when the game writes the STARTRENDER register. If MVC2 is hung at the BIOS or didn't autoboot, no frames. Confirm by tailing the flycast log for `[PVR]` messages.
- The `pvrQueue.enqueue(Render)` path requires `QueueRender(ctx)` to return true ([Renderer_if.cpp:440](../core/hw/pvr/Renderer_if.cpp#L440)). With norend, the queue should accept everything. If it doesn't, you broke `QueueRender` somehow.

### "It boots, frames go out, but the wire is byte-different from the GPU build"
- **You have a regression.** Run the determinism rig immediately. The eight bugs in [ARCHITECTURE.md:433-493](ARCHITECTURE.md#L433-L493) are the usual suspects.
- Check that `serverPublish()` is still called BEFORE `renderer->Process()` at [Renderer_if.cpp:197-198](../core/hw/pvr/Renderer_if.cpp#L197). If anyone reordered, the snapshot is wrong.
- Check that `_pvrAtomicSnap` is being populated. If the SPG scheduler thread isn't running (e.g. you broke the SH4 boot path), the snapshot will be stale.
- Check that `palette_update()` at [Renderer_if.cpp:442](../core/hw/pvr/Renderer_if.cpp#L442) is still firing. If you accidentally short-circuited `rend_start_render`, the palette won't update and the wire will diverge.

### "It boots, runs for ~30s, then segfaults"
- norend's `Render()` returns `!rendContext->isRTT` ([norend.cpp:18](../core/rend/norend/norend.cpp#L18)). If `rendContext` is somehow null (e.g. `RenderFramebuffer` was called and reset it), this dereferences null. Check the call sequence.
- `VramLockedWriteOffset` calls `mprotect`. If you're running under valgrind or strict seccomp, this can fail silently. Run without those layers to isolate.
- Save state load? `loadstate()` reinitializes the texture cache via the renderer ([ARCHITECTURE.md:519-521](ARCHITECTURE.md#L519)). With norend there's no texture cache, which may leave dangling pointers if not handled. Test with and without save state load.

### "It boots, hits 60fps, but the browser sees garbled rendering"
- This is a wire regression even though G4 might be passing for a short window. Run G4 over a longer period with a scene transition. Bug #6 (MAX_FRAME drop, [ARCHITECTURE.md:459-470](ARCHITECTURE.md#L459)) only manifests on post-SYNC keyframes which are infrequent.
- Confirm the relay isn't stale (hadn't been redeployed and is dropping new frame variants).
- Confirm browser cache buster is bumped if you touched the wasm.

### "The CMake build fails with undefined references after Phase 3"
- Trace each one. They are all real — something unconditionally references SDL/GL.
- Common culprits: `core/sdl/dreamlink/`, `core/sdl/dreamcast_link.cpp`, `core/input/keyboard_device.h` on SDL paths.
- Do NOT add empty stubs to silence linker errors. Either gate the caller or move the symbol behind `#if defined(USE_SDL)`.

### "imguiDriver is null and mainui_loop spins"
- Phase 1 step 5 should have fixed this. Verify your edit landed at [mainui.cpp:138](../core/ui/mainui.cpp#L138).
- If imguiDriver is null but `MAPLECAST_HEADLESS` is unset, that's a different bug — the GL context didn't initialize on a GPU box. Not your problem in this workstream.

### "It works locally but on the VPS it hits ~50fps"
- 2 vCPU is the minimum. Confirm the VPS isn't oversold (steal time on `top`). Verify with `vmstat 1` — `wa` should be ~0, `st` should be ~0.
- ZSTD compression is single-threaded per frame; if you only have 1 vCPU and the SH4 dynarec is also pegged, you'll miss the budget. Recommend 2 vCPU minimum.
- Confirm `config::ThreadedRendering` is false (default for headless) — threaded mode adds context-switch jitter that hurts on small VPSes.

### "I want to run multiple game instances on one VPS"
- Out of scope for this workstream. Each instance binds `:7100` and `:7200` — to multi-instance, you'd need to make those ports configurable per process and run multiple systemd units. Doable, but ship single-instance first. Document as a follow-up.

### "Can I run headless on macOS or Windows?"
- Phase 1 (runtime gate) probably works on macOS today because the env-var path doesn't compile-out anything.
- Phase 3 (CMake) is Linux-tested in this workstream. Mac/Windows may need additional gates (`win32_window_create`, etc.). Out of scope; document as a follow-up.

---

## 7. Out of Scope (Don't Do These In This Workstream)

- **Multi-instance per host.** Single-instance per VPS. Multi-instance is a follow-up.
- **Headless H.264 / NVENC.** That's a contradiction. NVENC requires a GPU. Document and move on.
- **Removing the GPU build.** The home box keeps its GPU build for local sub-1ms play. Both builds coexist from the same source tree.
- **Audio over the wire.** TA Mirror is video-only by design ([ARCHITECTURE.md "First Remote Test"](ARCHITECTURE.md)). Do not add audio in this workstream.
- **Renderer rewrites.** norend already exists. Use it. Do not "improve" it.
- **Cross-platform headless (macOS/Windows).** Linux only in this workstream.
- **Per-frame profiling improvements.** The publish budget is already documented and met. Don't optimize for fun.
- **Switching the home box to headless.** No. The home box renders locally for the cab's actual screen and for the player's preview. Different deployment. Keep them separate.

---

## 8. Definition of Done

All ten gates green. The headless flycast:

1. Builds with `cmake -DMAPLECAST_HEADLESS=ON .. && make` on Debian 12.
2. Has zero GL/SDL/X11 linkage in the resulting binary.
3. Runs on a CPU-only VPS with no `/dev/dri`, no `DISPLAY`, sustaining 60.0fps for 30+ minutes.
4. Produces wire bytes IDENTICAL to the GPU build, verified by the determinism rig over a multi-scene run.
5. Deploys via Docker + systemd in one command.
6. Survives crashes/restarts cleanly with auto-reconnect from browsers.
7. Is documented in `ARCHITECTURE.md` and `VPS-SETUP.md`.
8. Has a memory entry recorded so future agents know it shipped.
9. PR merged to `maplecast` branch with all eight bugs from `466d72d54` still verified-fixed.
10. The home box GPU build still works unchanged — verify one full match end-to-end on the home cab after merge.

**Until 1-10 are all green, the PR is not ready.** Ship the whole thing.
