/*
	MapleCast Mirror Client — Debug Overlay.

	Exposes a single ImGui draw function (no newFrame/endFrame) plus a
	tiny state singleton. The full frame lifecycle (newFrame, Render,
	renderDrawData) is owned by gui.cpp in gui_displayMirrorDebug(),
	which calls drawMirrorDebugContent() between the ImGui calls.

	This split exists so the drawing code lives in its own TU (isolated
	from gui.cpp's many responsibilities) but can still use gui.cpp's
	file-static imguiDriver and the static gui_newFrame/gui_endFrame
	helpers.
*/
#pragma once

namespace gui_mirror_debug
{

// Toggled by Tab in the main render loop. Atomic-safe read is fine — the
// race window is a single frame.
bool isVisible();
void setVisible(bool v);
void toggleVisible();

// Drawing body. Call between ImGui::NewFrame() and ImGui::Render().
// Owns the ImGui windows, reads telemetry atomics, handles button
// callbacks that mutate live state (latch policy, audio enable, reconnect).
void drawContent();

// Feed one raw log line into the overlay's scrolling log ring. The ring
// is bounded (~128 lines, drops the oldest). Called by runtime systems
// that want their messages visible in the overlay without also needing to
// grep stdout.
void logLine(const char* text);

} // namespace gui_mirror_debug
