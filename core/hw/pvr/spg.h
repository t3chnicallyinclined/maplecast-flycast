#pragma once
#include "ta_ctx.h"

extern bool SH4FastEnough;

bool spg_Init();
void spg_Term();
void spg_Reset(bool Manual);
void spg_Serialize(Serializer& ser);
void spg_Deserialize(Deserializer& deser);

void CalculateSync();
void read_lightgun_position(int x, int y);
void scheduleRenderDone(TA_context *cntx);
void rescheduleSPG();

// Most recent jitter passed to spg_line_sched (vblank_schid callback).
// Read by the rollback ring's saveFrame to reconstruct handle_cb's
// post-callback re-schedule on rewind (closes 2 bytes of byte-diff drift).
extern thread_local int spg_last_jitter;
int spg_getNextInterrupt(); // wrapper for getNextSpgInterrupt() —
                            // exposes the next-scanline-cycle calculation
                            // to the rollback ring's rewind path.
