/*
===========================================================================

dhewm 3 on eacp - the window's contents, and the engine's host.

Phase 2 of ../../../plan.md. The engine is started from here rather than from
main() because the renderer will want a window to come up in (step 4), and
update() is the first place there certainly is one: the view is in the window,
sized, and its Metal layer exists.

Today the engine runs headless - com_skipRenderer 1, see View.cpp - so what is
on screen is still a cleared window. What is different is that behind it the
file system, the sound system, the game library and the session are all up, and
common->Frame() is running once a refresh.

===========================================================================
*/

#ifndef __SYS_EACP_VIEW_H__
#define __SYS_EACP_VIEW_H__

#include <eacp/GPU/GPU.h>

namespace dhewm3
{
using namespace eacp;

struct View final : GPU::GPUView
{
	View();
	~View() override;

	void update(Threads::FrameTime) override;
	void render(GPU::Frame& frame) override;

private:
	// common->Init, once, on the first refresh. It reads pk4s and loads the
	// game library, so it takes a second or two of that refresh; the display
	// link coalesces the ticks it misses.
	void startEngine();

	bool engineStarted = false;
};
} // namespace dhewm3

#endif /* !__SYS_EACP_VIEW_H__ */
