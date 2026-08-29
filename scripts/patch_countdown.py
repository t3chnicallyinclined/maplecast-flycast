#!/usr/bin/env python3
# Fix the MCSV build countdown: checkMatchEnd runs at 1 Hz, so 300 meant 5
# MINUTES, not 5s. Set to 6 (~6s). Server-only change.
path = "/opt/maplecast/src/core/network/maplecast_ws_server.cpp"
old = (
    "\t\t\t_mcsvBuildCountdown = 300;\n"
    "\t\t\tprintf(\"[maplecast-ws] MCSV build scheduled in 300 frames\\n\");"
)
new = (
    "\t\t\t// NOTE: decremented in checkMatchEnd on the 1 Hz status thread, so the\n"
    "\t\t\t// unit is SECONDS not frames. 300 meant 5 MINUTES (MCSV never built in\n"
    "\t\t\t// a fresh match); 6 = ~6s, clearing the round-intro disc I/O.\n"
    "\t\t\t_mcsvBuildCountdown = 6;\n"
    "\t\t\tprintf(\"[maplecast-ws] MCSV build scheduled in 6s (round-intro disc I/O guard)\\n\");"
)
with open(path, "r", encoding="utf-8") as f:
    d = f.read()
n = d.count(old)
if n != 1:
    print(f"ABORT: found {n} matches")
    raise SystemExit(1)
with open(path, "w", encoding="utf-8") as f:
    f.write(d.replace(old, new, 1))
print("OK: countdown patched to 6s")
