# MapleCast Latency Optimization Plan

> The goal: **sub-25ms button-to-pixel** over public internet to a Vultr-hosted community node — matching or beating an arcade cab. Today's baseline is ~37ms.
>
> Owner: Tristan. Created: 2026-05-07.

---

## Current latency budget (baseline)

Measured on the native Windows mirror client connecting to `ord.nobd.net` (Chicago) from NJ residential broadband:

| Component | Time | Notes |
|---|---|---|
| Gamepad → SDL → UDP send (Win) | ~2-3ms | SDL adds 1-3ms vs. Raw Input |
| UDP NJ → Chicago (one-way) | ~10ms | half of 21ms RTT |
| Server input latch + emulate frame | ~5ms | SH4 JIT + TA serialize |
| Compress + WS send | ~80µs + 10ms | zstd lvl 1 + return path |
| Client decompress + render | ~2ms | per-frame logged |
| Display present (vsync) | ~8ms avg | 60Hz, half-frame avg |
| **Total button-to-pixel** | **~37-40ms** | over current path |

For reference: a real MVC2 cab is ~25ms. We're 12-15ms over. **All of that delta is fixable.**

---

## What's already optimized (don't re-relitigate)

- ✅ TCP_NODELAY on relay/mirror WS
- ✅ LTO + native CPU tuning in Release builds
- ✅ Hugepages (vm.nr_hugepages=256)
- ✅ SO_BUSY_POLL, busy-read sysctl (50µs)
- ✅ AF_XDP input ingress (copy mode — virtio_net doesn't support zero-copy)
- ✅ SCHED_FIFO on hot threads + ambient capabilities
- ✅ evdev direct input bypass (Linux server side)
- ✅ 19-byte timestamped UDP wire + atomic seq+ts input layer
- ✅ Six race-condition fixes preventing wire stalls (commit 466d72d54)
- ✅ Native client uses port 7200 direct (skips relay's spectator-fanout path)
- ✅ Audio on separate socket (cannot HOL-block frames)
- ✅ MAPLECAST_NO_FAILOVER env to suppress spurious 100ms-jitter failover (no state sync between nodes yet)

---

## Tier 1 — cheap wins, server-side (free, ~30 min total)

### 1. Geographic optimization: spin up an EWR community node
Northeast users currently hit `ord` (Chicago) at 21ms. An EWR (NJ) community node would be **~3-8ms** RTT — roughly half the current latency. Same provisioning script as the existing 4 nodes. ~5 min, $6/mo.

**This is the cheapest 10ms+ saving available.**

### 2. CPU governor → performance on all community nodes
```bash
echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```
Eliminates dynamic frequency-scaling pauses. ~50-200µs jitter reduction per frame.
Persist via `cpupower frequency-set -g performance` or a `tuned` profile.

### 3. Pin DMA latency to 0
```bash
# Daemonized: hold /dev/cpu_dma_latency open with value 0
nohup bash -c 'exec 3>/dev/cpu_dma_latency; printf "\\x00\\x00\\x00\\x00" >&3; sleep infinity' &
```
Vultr KVM exposes some C-states; pinning DMA latency to 0 keeps cores hot, eliminates wake-up jitter.

### 4. NIC interrupt coalescing off + GRO/LRO off
```bash
IFACE=$(ip route get 8.8.8.8 | awk '{print $5; exit}')
ethtool -C $IFACE rx-usecs 0 tx-usecs 0
ethtool -K $IFACE gro off lro off
```
Saves up to 50µs per packet on input/frame paths.

### 5. CPU affinity / pinning
Pin flycast SH4 thread to core 0, relay to core 1 via systemd `CPUAffinity=`. Better cache residency, no migrations.

### 6. Bigger NIC ring buffers
```bash
ethtool -G $IFACE rx 4096 tx 4096   # virtio cap typically 1024-4096
```
Absorbs burst without dropping under sustained load.

### 7. Verify SCHED_FIFO is actually applied
```bash
chrt -p $(pgrep -o flycast)
# expect: SCHED_FIFO with priority > 0
# if SCHED_OTHER, capability grants aren't taking effect
```

**Estimated total Tier 1 win: ~10-15ms (mostly from #1).**

---

## Tier 2 — Windows client tweaks (1-3 days of work)

### 8. Raw Input API replacing SDL gamepad
Listed as a known limitation in [WINDOWS-CLIENT-BUILD.md:292](WINDOWS-CLIENT-BUILD.md#L292). Bypasses SDL's event queue. Native Win32 `RegisterRawInputDevices` + `WM_INPUT` handler. Linux client already does this via evdev. **1-3ms saving.**

### 9. DXGI Independent Flip mode (true borderless fullscreen)
Bypass the DWM compositor. `IDXGISwapChain::SetFullscreenState` + flip discard model. Currently the renderer is OpenGL via SDL — would need a DXGI backend or flip-model wrapper. **5-15ms saving** — biggest single Windows-side win.

### 10. MMCSS Pro Audio class for the audio recv thread
```c
DWORD task_idx = 0;
HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_idx);
```
Lowest-jitter scheduling on Windows. Smoother than `THREAD_PRIORITY_TIME_CRITICAL`.

### 11. VirtualLock + SetProcessWorkingSetSize
Pin all working pages so no page-fault stalls during gameplay. ~1-2 page-fault events per minute on Windows otherwise; each is 100µs-1ms.

### 12. GPU driver tweaks (manual, user-side)
NVIDIA Control Panel:
- **Low Latency Mode**: Ultra
- **Power management mode**: Prefer maximum performance
- **Vertical sync**: Off
- **Threaded optimization**: On

AMD has equivalent toggles. Easy 1-3ms.

### 13. Disable Windows Defender real-time scan for `flycast.exe`
Add an exclusion in Windows Security. Eliminates per-IO scan overhead. ~100µs–1ms.

### 14. Game Mode + Ultimate Performance power plan
```
powercfg /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c
```

**Estimated total Tier 2 win: ~10-20ms (Independent Flip is the big one).**

---

## Tier 3 — the crazy stuff

### 15. 🔥 Rollback prediction over TA mirror — GGPO-style
**The holy grail.** Feasible because of byte-perfect determinism.

1. Client runs its own SH4 emulator IN PARALLEL with the server
2. When you press a button, client predicts the frame locally **immediately** — input feels instant
3. Server's authoritative frame arrives 20ms later
4. If predicted == authoritative, no-op. If mismatch (other player did something), client rolls back state and re-simulates with the new input log

**Result: input feels like ~5ms (local emulation) even on a 50ms server.**

Architecture:
- Reuse `.mcrec` deterministic replay infrastructure (Phase 4)
- Client embeds a flycast SH4 emulator (already exists — just don't disable CPU like the mirror does)
- Diff client-predicted TA against server-authoritative TA, rollback on mismatch
- Estimated effort: ~2-3 weeks. **Massive payoff. No fighting-game streaming setup has this.**

### 16. Multi-path racing for TA frames
Send each TA frame over 2-3 transports simultaneously (WS over Caddy + WebTransport over QUIC + raw UDP-fragmented), use whichever copy arrives first per frame. Eliminates single-path packet loss + head-of-line blocking. 2× bandwidth cost (still nothing on a 4 Mbps stream).

### 17. Speculative emulation pacing
Client SH4 emulator runs **slightly ahead** of the server. When server frame arrives, server's frame is the source of truth, but the client has already drawn the next frame. ~1-2ms saving on render schedule.

### 18. Bare-metal colo near you
$50-150/mo for 1U colo box at NJ datacenter near the player. **RTT = 1-3ms** instead of 5-10ms. Look at OVH Vint Hill VA, INAP NJ, Hivelocity NJ.

### 19. eBPF input UDP fast-path
Kernel-side eBPF filter that intercepts input UDP packets *before* they hit the userspace socket queue. Direct map to a hugepage shared with flycast. Saves ~50-100µs per packet.

### 20. 240Hz/360Hz client display + Independent Flip
Render at 60fps from server, but **present** at 240Hz. Average display latency drops from 8ms to 2ms.

### 21. Photodiode display calibration
$20 phototransistor + Arduino. Flash the screen, measure actual pixel response. Bake into HUD's E2E number. True button-to-photons reading.

---

## Recommended execution order

| # | Action | Wall time | Latency win |
|---|---|---|---|
| 1 | Spin up EWR community node | 5 min | ~10ms |
| 2 | CPU gov + DMA latency + IRQ coalescing on all 5 nodes | 10 min | ~1-2ms |
| 3 | NVIDIA driver tweaks on user box | 5 min | ~1-3ms |
| 4 | Verify SCHED_FIFO is taking effect on nodes | 10 min | sanity check |
| 5 | Tournament mode env var (RawInput + High Perf power plan + working-set lock) | 1-2 days | ~3-5ms |
| 6 | **Rollback prediction (GGPO over TA)** | 2-3 weeks | **~30ms perceived** |

Steps 1-4 in 30 min puts the cab user at ~20-22ms button-to-pixel. **Cab-class.**

Step 6 is the moonshot — what makes MapleCast genuinely best-in-class.

---

## Status (this doc tracks completion as work lands)

- [x] **#1 EWR node provisioned + registered (2026-05-07)** — `maplecast-ewr` (45.77.147.101), DNS `ewr.nobd.net`, TLS via Caddy/LE, all 3 endpoints (`/ws`, `/play`, `/audio`) HTTP 101. Status `ready`.
- [x] **#2 Tier 1 sweep on all 5 community nodes (2026-05-07)** — DMA-latency hold daemon (`/etc/systemd/system/dma-latency-hold.service`) active, ethtool `rx-usecs 0 / gro off / lro off / rx-ring 4096`, persistent via `maplecast-tuning.service`. CPU governor not available (Vultr KVM exposes no cpufreq sysfs — managed by hypervisor). CPU affinity skipped on 1-vCPU plans. **Hugepages reduced from 256 → 64** (was OOM-killing flycast on 1GB instances during savestate load).
- [ ] #3 NVIDIA Control Panel toggles + Windows power plan + Defender exclusion (manual user step — see "Tier 2 quick wins" section below)
- [x] **#4 SCHED_FIFO verification** — confirmed working: the latency-critical network thread runs `SCHED_FIFO priority 55` on all community nodes (only the main flycast thread is intentionally `SCHED_OTHER`; the hot thread is correctly escalated via `xdp.conf` ambient capabilities).
- [ ] #5 Tournament mode env var (Windows client) — see scope notes below
- [ ] #6 Rollback prediction
  - [x] **Phase 0** — determinism rig **fully validated 2026-05-07** (see ARCHITECTURE.md "Cross-machine + cross-OS SH4 determinism" section)
    - Step A wire faithfulness 48/48 ✓
    - Step B same-machine determinism 30/30 ✓
    - Step C-Linux cross-Linux-machine 19/19 ✓
    - Step C-Windows cross-OS Linux↔Windows 30/30 ✓
    - **Step D `.mcrec` replay-mode determinism 16/16 ✓** (live-recorded on Linux server with real gameplay inputs, replayed on Windows via `replay_reader.cpp`, byte-identical TA per frame)
  - [ ] Phase 1 — shadow-mode SH4 thread in mirror client
  - [ ] Phase 2 — naïve prediction (renderer reads from local SH4)
  - [ ] Phase 3 — rollback ring + comparator
  - [ ] Phase 4 — performance + observability HUD
  - [ ] Phase 5 — polish, opt-in/opt-out, docs

---

## Notes from execution

### vhf-1c-1gb plan is borderline for flycast
1 GB total RAM. Flycast wants ~410 MB (anon + file + 272 MB shmem TA ring). Plus Caddy + relay + sysd ≈ ~600 MB working set. With `vm.nr_hugepages=256` (512 MB pre-reserved) we ran out of RAM during savestate load (27 MB compressed → ~50 MB hot). Reducing hugepages to 64 (128 MB) is the fix. **If we ever bump capacity, vhf-1c-2gb at $12/mo eliminates this entirely.**

### CPU governor on Vultr KVM
`/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor` is not exposed inside Vultr KVM guests. Frequency is fully hypervisor-controlled — guests can't request "performance" governor. The Tier 1 #2 line item is a no-op on this provider. On bare metal it would matter; here it doesn't.

### Tier 2 quick wins (no code, manual user steps)

**Power plan (Windows):**
```powershell
powercfg /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c   # Ultimate Performance
# If not present: powercfg -duplicatescheme e9a42b02-d5df-448d-aa00-03f14749eb61
```

**NVIDIA Control Panel** (right-click desktop → NVIDIA Control Panel → Manage 3D settings → Program Settings → add `flycast.exe`):
- Low Latency Mode: **Ultra**
- Power management mode: **Prefer maximum performance**
- Vertical sync: **Off**
- Threaded optimization: **On**

**Windows Defender exclusion:**
Settings → Windows Security → Virus & threat protection → Manage settings → Add or remove exclusions → File → `C:\Users\trist\projects\maplecast-flycast\build\flycast.exe`

**Set Game Mode on:**
Settings → Gaming → Game Mode → On

Net win: ~3-8ms reduction in jitter from removing background CPU/IO interference and pre-empting Windows Defender's per-IO scan.

### #5 Tournament mode env var — scope split

The "Tournament Mode" idea has three sub-pieces with very different effort levels:

1. **Process-level tweaks** ✅ done. `MAPLECAST_TOURNAMENT_MODE=1` env var → in-process: HIGH priority class, working-set lock 192-384 MB, disable priority boost.

2. **Win32 RawInput gamepad bypass** ✅ done. `MAPLECAST_RAWINPUT=1` (already shipped on master via `03bec497b`) — XInput direct polling at 1 kHz on `THREAD_PRIORITY_TIME_CRITICAL`, with Raw Input fallback for non-XInput sticks. Wizard at `MAPLECAST_BUTTON_WIZARD=1` for one-time button mapping.

3. **DXGI Independent Flip mode** — see #5.3 plan below.

---

### #5.3 — DXGI Independent Flip plan (Windows-only)

DXGI is Windows-only. Linux equivalent (DRM/KMS direct mode + Wayland tearing-control) and macOS equivalent (`kCGDisplayCaptureFlags`) are independent ~2-day efforts each.

**Discovery (2026-05-07)**: most plumbing is ALREADY in the codebase. The DX11 backend (`USE_DX11=ON`, `HAVE_D3D11`) is compiled into the client binary. [`core/rend/dx11/dx11context.cpp`](../core/rend/dx11/dx11context.cpp) already creates a DXGI swap chain with `DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL` and `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`. We're tuning, not building. Revised estimate: **1.5-2 days**.

**Knobs to flip:**
| Knob | Current | Want | Win |
|---|---|---|---|
| Mirror-client renderer on Win | OpenGL (via SDL) | DX11 | enables the rest |
| `IDXGIDevice1::SetMaximumFrameLatency` | not called (default 3) | **`(1)`** | up to **33ms** of queue latency removed — biggest single win |
| `SwapEffect` | `FLIP_SEQUENTIAL` | `FLIP_DISCARD` | mailbox-style: present newest, drop stale |
| Window state | bordered windowed | borderless fullscreen at native res | Independent Flip eligibility requirement |
| Verification | none | PresentMon log + `DwmGetCompositionTimingInfo` | confirm hardware path |

**Phases:**
1. **(½ day) Switch mirror client to DX11** — `core/emulator.cpp` picks renderer based on env / build mode. Keep OpenGL as fallback if DX11 init fails.
2. **(½ day) Tune swapchain in `dx11context.cpp`** — `SetMaximumFrameLatency(1)`, `FLIP_DISCARD` (gated on TOURNAMENT_MODE), verify ALLOW_TEARING fires.
3. **(½ day) Borderless fullscreen at monitor's native res** — `core/sdl/sdl.cpp` window flags + `SDL_SetWindowBordered(SDL_FALSE)`. New env var `MAPLECAST_BORDERLESS=1` (auto under TOURNAMENT_MODE).
4. **(½ day) Verification** — PresentMon trace + log line via `DwmGetCompositionTimingInfo` reporting compositor state every 5s.

**Acceptance criteria:**
1. Console: `[render] DX11 backend`, `MaxFrameLatency=1`, `FLIP_DISCARD`, `borderless fullscreen WxH`
2. PresentMon shows `Hardware: Independent Flip` ≥95% of frames
3. F2 HUD frame→pixel drops ≥5ms vs OpenGL
4. No regression on char select / stage select / hyper flashes

**Risks:**
- DX11 backend was designed for full flycast pipeline; mirror-client mode (TA from wire) may hit untested code paths → OpenGL fallback
- NVIDIA Optimus laptops sometimes refuse Independent Flip (dGPU/iGPU compositor switch) → detect via `DXGI_FRAME_STATISTICS_PRESENT_FAILURE`
- VRR (G-Sync/FreeSync) + flip + tearing: actually a bonus — freerun, no tearing, no vsync wait
