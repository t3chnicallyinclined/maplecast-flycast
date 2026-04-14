# Native Mirror Client — Tuning for Tournament Cabs

How to squeeze the lowest possible E2E latency and jitter out of the
native flycast mirror client (`MAPLECAST_MIRROR_CLIENT=1`) on a
physical-cab Linux box.

Baseline: 10 ms E2E avg on a clean LAN. With the tunings below, worst-case
jitter drops from ~2 ms to <50 µs and the average trends toward ~6–8 ms.

---

## 1. Grant CAP_SYS_NICE so SCHED_FIFO works

The trigger-poll thread ([core/network/maplecast_input_sink.cpp](../core/network/maplecast_input_sink.cpp)
`triggerPollLoop`) runs at SCHED_FIFO priority 90 to avoid desktop
compositor preemption. Setting that priority requires `CAP_SYS_NICE`.

Three ways to grant it:

```bash
# Option A — set file capability on the binary (recommended, survives rebuild-and-reinstall)
sudo setcap cap_sys_nice+ep /path/to/flycast

# Option B — run as root (not recommended for production cabs)
sudo ./flycast --server 192.168.1.10

# Option C — add the cab user to a group with rtprio limits
# In /etc/security/limits.conf:
#   @cabops - rtprio 90
# Then log out and back in.
```

Verify at runtime. On startup you should see:

```
[input-sink] trigger poll thread → SCHED_FIFO prio 90
```

If you see the fallback message (`SCHED_FIFO unavailable`), the capability
isn't applied. The client still runs — just with the old jitter
characteristics.

---

## 2. Isolate a CPU core (isolcpus)

Dedicate a core to the poll thread + UDP send so no other userland or
kernel work can preempt it.

### One-time kernel cmdline

Edit `/etc/default/grub`, append to `GRUB_CMDLINE_LINUX_DEFAULT`:

```
isolcpus=3 nohz_full=3 rcu_nocbs=3
```

Then:

```bash
sudo update-grub  # or grub2-mkconfig -o /boot/grub2/grub.cfg on RHEL/Fedora
sudo reboot
```

Verify on reboot:

```bash
cat /sys/devices/system/cpu/isolated   # should show "3"
```

### Pin the flycast process to the isolated core

```bash
taskset -c 3 ./flycast --server 192.168.1.10
```

(The `taskset` invocation moves the main thread and every thread it
spawns, including `triggerPollLoop`, onto core 3. The rest of the system
continues running on cores 0–2.)

---

## 3. Reduce compositor latency (optional but effective)

The desktop compositor adds 0–16 ms of dwell time between
`eglSwapBuffers` and the monitor receiving the pixel. Options:

- **Run without a compositor.** On Xorg, use a minimal WM like `twm` or
  `i3` with all compositors disabled. On Wayland, pick a compositor with
  a tearing / low-latency mode (KWin ≥ 5.27 supports tearing, sway has
  `allow_tearing yes`).
- **Run fullscreen.** Fullscreen windows on most compositors bypass
  composition and go straight to scan-out. Flycast's fullscreen toggle
  (F11 or cli `-fullscreen`) is enough.
- **Pin refresh rate.** If the display supports VRR, disable it for
  tournament play. VRR introduces irregular scan-out timing.

---

## 4. Network tuning for UDP input

The client sends button state to the server via UDP port 7100. Two
small kernel tunables reduce tail latency on busy interfaces:

```bash
# Increase UDP send buffer (defaults are ~200 KB; go to 4 MB for headroom)
sudo sysctl -w net.core.wmem_max=4194304
sudo sysctl -w net.core.wmem_default=1048576

# Reduce UDP receive jitter on the LAN card
sudo sysctl -w net.core.netdev_max_backlog=30000
```

Persist in `/etc/sysctl.d/99-maplecast.conf`.

If the cab has an Intel or RealTek NIC, also disable interrupt
coalescing for input-carrying packets:

```bash
sudo ethtool -C eth0 rx-usecs 0 tx-usecs 0
```

---

## 5. Verify the tuning worked

After applying all of the above, measure:

### Poll thread priority
```bash
pgrep -t <flycast-pid> | xargs -I{} chrt -p {} 2>&1 | grep SCHED_FIFO
```

Should show the poll thread (triggerPollLoop) at SCHED_FIFO priority 90.
Other threads stay at SCHED_OTHER — that's correct.

### Core affinity
```bash
taskset -p <flycast-pid>  # should show "cpus list: 3"
```

### Actual jitter
Use the client's built-in E2E latency probe (exposed via the Back button
or the gear-icon settings panel — see `web/client-settings.html`). Watch
the `min`, `avg`, `max` over a 60-second idle period:

- **Pre-tuning:** max − min often 1–3 ms
- **Post-tuning:** max − min typically < 200 µs

If you still see high max values, chase them:
1. `dmesg | grep -i latency` for kernel preemption events
2. `perf sched record / perf sched latency` for scheduler traces
3. Make sure no background tasks (backups, cron jobs, screen recorders)
   are hitting the isolated core

---

## 6. What's coming (not yet shipped — see ultra-low-latency plan)

- **C3 — Velocity-based client prediction.** Reads `vel_x`/`vel_y` from
  the 253-byte game state and extrapolates sprite positions forward one
  frame. Should hide 8–10 ms of network latency for steady-state motion
  (walking, falling, cancellable moves). Will gate behind
  `MAPLECAST_DISABLE_PREDICTION=1` for rollback.
- **C4 — STARTRENDER-timed presentation.** Bypass compositor dwell by
  scheduling `eglSwapBuffers` relative to the server's STARTRENDER
  timestamp (shipped in frame metadata). Requires DRM page-flip access
  or a non-compositing display path.
- **C2 — Maple 12-byte wire format.** Replace the custom 7-byte UDP
  packet with the Maple CMD9 response wire format for zero-copy
  server-side ingestion. Saves ~1 µs per packet — marginal but cleans
  up the protocol.

---

## 7. Related docs

- [docs/ARCHITECTURE.md](ARCHITECTURE.md) — overall system topology
- [docs/INPUT-LATCH.md](INPUT-LATCH.md) — server-side latch policies and
  the 300 µs guard window
- [docs/MVC2-MEMORY-MAP.md](MVC2-MEMORY-MAP.md) — the 253-byte state
  consumed by prediction (C3)

---

*Doc created 2026-04-14 alongside C1 (SCHED_FIFO poll thread). Update
whenever new client-side tunables ship or a measurement threshold
changes meaningfully.*
