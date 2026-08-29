#!/usr/bin/env python3
# Surgically apply the emu-thread MCSV-capture fix to the VPS source tree.
# Each replacement must match exactly once or we abort (no partial patch).
import sys

SRC = "/opt/maplecast/src/core/network"
H   = f"{SRC}/maplecast_ws_server.h"
CPP = f"{SRC}/maplecast_ws_server.cpp"
MIR = f"{SRC}/maplecast_mirror.cpp"

patches = []

# --- H1: ws_server.h — declare drainMcsvCapture() ---
patches.append((H,
"void broadcastBinary(const void* data, size_t size);",
"""void broadcastBinary(const void* data, size_t size);

// Drain a pending MCSV (mid-match savestate) capture request. MUST be called
// from the EMU THREAD at a frame boundary (serverPublish): dc_serialize reads
// live SH4 registers, and capturing from the 1Hz status thread mid-execution
// can snapshot SR.BL=1 (mid-interrupt), crashing replica clients on load with
// "SH4 exception when blocked". No-op unless checkMatchEnd requested a capture.
void drainMcsvCapture();"""))

# --- H2: ws_server.cpp — atomic request flag ---
patches.append((CPP,
"static int _mcsvBuildCountdown = 0; // frames until deferred MCSV build fires",
"""static int _mcsvBuildCountdown = 0; // frames until deferred MCSV build fires

// Set by checkMatchEnd (status thread) when the countdown fires; drained by
// drainMcsvCapture() on the emu thread so dc_serialize runs at a clean SH4
// frame boundary (SR.BL=0) instead of mid-interrupt.
static std::atomic<bool> _mcsvCaptureRequested{false};"""))

# --- H3: ws_server.cpp — drainMcsvCapture() definition ---
patches.append((CPP,
"""\tprintf("[maplecast-ws] MCSV cached: %.1f MB raw -> %.1f MB compressed\\n",
\t       stateSize / (1024.0*1024.0), compSize / (1024.0*1024.0));
}""",
"""\tprintf("[maplecast-ws] MCSV cached: %.1f MB raw -> %.1f MB compressed\\n",
\t       stateSize / (1024.0*1024.0), compSize / (1024.0*1024.0));
}

// Emu-thread entry: called once per frame from serverPublish. Runs the actual
// dc_serialize capture ONLY when checkMatchEnd has requested it -- at this point
// the SH4 is between frames (SR.BL=0), so the savestate is clean and replica
// clients can resume it without "SH4 exception when blocked".
void drainMcsvCapture()
{
\tif (_mcsvCaptureRequested.exchange(false, std::memory_order_acquire)) {
\t\tbuildMcsvCache();
\t\tprintf("[maplecast-ws] MCSV built on emu thread (clean SH4 boundary, SR.BL=0)\\n");
\t}
}"""))

# --- H4: ws_server.cpp — countdown sets flag instead of capturing ---
patches.append((CPP,
"""\t\t// Deferred MCSV build -- fires 300 frames after match-start.
\t\tif (_mcsvBuildCountdown > 0 && --_mcsvBuildCountdown == 0) {
\t\t\tbuildMcsvCache();
\t\t\tprintf("[maplecast-ws] MCSV built (300-frame delay complete)\\n");
\t\t}""",
"""\t\t// Deferred MCSV build -- fires 300 frames after match-start. We DON'T
\t\t// capture here (1Hz status thread): dc_serialize reads live SH4 regs and
\t\t// can snapshot SR.BL=1 mid-interrupt, crashing replica clients on load
\t\t// ("SH4 exception when blocked"). Request it; serverPublish (emu thread,
\t\t// between frames, SR.BL=0) runs the actual build.
\t\tif (_mcsvBuildCountdown > 0 && --_mcsvBuildCountdown == 0) {
\t\t\t_mcsvCaptureRequested.store(true, std::memory_order_release);
\t\t\tprintf("[maplecast-ws] MCSV capture requested -- emu thread builds at next frame boundary\\n");
\t\t}"""))

# --- H5: mirror.cpp — drain on emu thread inside serverPublish ---
patches.append((MIR,
"""\tif (!maplecast_ws::active() || maplecast_ws::clientCount() == 0) {
\t\t// Update frame counter + basic telemetry for local overlays
\t\t_atomicCurrentFrame.store(_localFrameNum, std::memory_order_release);""",
"""\t// Emu-thread MCSV capture: dc_serialize must run here (between frames,
\t// SR.BL=0), NOT on the status thread that requests it -- a mid-interrupt
\t// snapshot crashes replica clients on load ("SH4 exception when blocked").
\t// Cheap atomic check every frame; the heavy serialize fires once per match.
\tif (maplecast_ws::active())
\t\tmaplecast_ws::drainMcsvCapture();

\tif (!maplecast_ws::active() || maplecast_ws::clientCount() == 0) {
\t\t// Update frame counter + basic telemetry for local overlays
\t\t_atomicCurrentFrame.store(_localFrameNum, std::memory_order_release);"""))

ok = True
for path, old, new in patches:
    with open(path, "r", encoding="utf-8") as f:
        data = f.read()
    n = data.count(old)
    if n != 1:
        print(f"ABORT: {path}: expected 1 match, found {n}")
        ok = False
        continue
    data = data.replace(old, new, 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(data)
    print(f"OK: patched {path}")

sys.exit(0 if ok else 1)
