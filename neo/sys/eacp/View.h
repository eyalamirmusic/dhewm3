/*
===========================================================================

dhewm 3 on eacp - the window's contents.

Phase 2 of ../../../plan.md. This grows into the engine's host: update() will
step common->Frame() off the display link, and render() will run the backend's
command list. Today it opens, clears, and does nothing else.

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

	void render(GPU::Frame& frame) override;
};
} // namespace dhewm3

#endif /* !__SYS_EACP_VIEW_H__ */
