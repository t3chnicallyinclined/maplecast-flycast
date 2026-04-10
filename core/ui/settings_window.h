#pragma once

namespace settings_window
{
	void init();      // Create window + renderer (call on main thread)
	void tick();      // Draw one frame (call from main loop)
	void show();
	void hide();
	void toggle();
	void shutdown();
}
