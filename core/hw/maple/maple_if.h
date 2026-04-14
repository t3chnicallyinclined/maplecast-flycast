#pragma once
#include "maple_devs.h"
#include <memory>

extern std::shared_ptr<maple_device> MapleDevices[MAPLE_PORTS][6];

void maple_Init();
void maple_Reset(bool Manual);
void maple_Term();
void maple_ReconnectDevices();
void maple_ReconnectDevice(int bus, int port);

void maple_vblank();

// S3 — measured microsecond offset from the VBlank-IN edge (entry to
// maple_vblank) to the CMD9 kick (the ggpo::getInput call inside
// maple_DoDma). This is the "last safe moment" for an input packet to
// land in kcode[] and still be read by the game for the upcoming frame.
//
// MVC2 writes SB_MDST=1 from its VBlank-IN ISR; the actual µs offset is
// game-specific and previously unmeasured. These accessors return a
// smoothed EMA and the peak-since-reset. Zero until the first measurement.
//
// Used by the input latch path to schedule near-boundary packet writes
// precisely (S7) and published in frame_phase status JSON for diagnostics.
int64_t maple_getVblankKickOffsetUsEma();
int64_t maple_getVblankKickOffsetUsMax();
int64_t maple_getVblankKickOffsetUsLast();
void    maple_resetVblankKickOffsetPeak();
