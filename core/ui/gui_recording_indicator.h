// gui_recording_indicator.h
//
// Native F9 hotkey + on-screen "[REC mm:ss]" overlay for the flycast
// .mcrec record/replay system. Replaces the PowerShell scripts for the
// common case where the user is playing in a flycast window.
//
// toggle(): start recording if idle, stop if active. Auto-generates an
// output path under <user>/Documents/MapleCastReplays/. Wraps the same
// emu.stop() -> writer::start() (dc_savestate slot 99 + embed +
// dc_loadstate) -> emu.start() bracket the control-WS endpoint uses, so
// the V2 byte-deterministic invariant is preserved.
//
// draw(): called once per ImGui frame from gui.cpp's overlay sequence.
// No-op when not recording.

#pragma once

namespace gui_recording_indicator {

void toggle();
void draw();

} // namespace gui_recording_indicator
