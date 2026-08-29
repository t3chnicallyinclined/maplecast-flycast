// MAPLECAST_AUTOSELECT — deterministic char-select driver for MvC2 (retail DC / mvsc2).
// From a char-select savestate, navigates BOTH players' cursors to a target team and
// locks them, then hands off to the match-input movie. Reads flycast guest RAM (mem_b)
// exactly like the king.html state reader; injects DC-pad inputs via getLocalInput.
//
// Env:  MAPLECAST_AUTOSELECT=<p1a,p1b,p1c>;<p2a,p2b,p2c>   (char_ids, dec or 0xHEX)
// Spec: char-select grid table @guest 0x8C161FEC (char_id=tbl[row*8+col], 0xFF=empty);
//       cursor col P1=0x8C28C414/P2=0x8C28C415, row P1=0x8C28C416/P2=0x8C28C417;
//       lock byte P1=0x8C28C412/P2=0x8C28C413 (0x77=that side done);
//       confirm = any attack button; one d-pad EDGE = jump to next valid untaken cell.
#pragma once
#include <cstdint>

namespace maplecast_autoselect {
// True once MAPLECAST_AUTOSELECT is parsed and we haven't finished the handoff.
bool active();
// For player 0/1 during char-select/hand-off: fills `kcodeLow` (active-low DC_BTN low-16)
// with the input to inject THIS frame and returns true. Returns false when this player's
// selection is finished (fall through to the normal input path / match movie).
bool getInput(int player, uint16_t& kcodeLow);
// Call once per vblank; drives the state machine off guest RAM. Returns true while the
// movie pace clock must stay held (char-select + intro not yet at round start).
bool holdMoviePace();
}
