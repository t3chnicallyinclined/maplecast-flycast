/*
	MapleCast Client-Side Palette Override.

	Overrides PVR palette RAM entries locally on the client before
	each frame renders. Zero server involvement for display — the
	client stores its own custom palette and applies it every frame
	after the TA stream writes the server's palette but before the
	renderer reads it.

	Set overrides via the control WS "client_palette_set" command.
*/
#pragma once
#include <cstdint>

namespace maplecast_palette
{
	// Apply all active overrides to pvr_regs PALETTE_RAM.
	// Call once per frame, after clientReceive, before Render.
	void applyClientOverrides();

	// Set a client-side palette override (called from control WS).
	// index: PVR palette entry start (0-1023)
	// colors: array of ARGB4444 u16 values
	// count: number of entries
	void setOverride(int index, const uint16_t* colors, int count);

	// Clear all client-side overrides.
	void clearOverrides();

	// True if any overrides are active.
	bool hasOverrides();
}
