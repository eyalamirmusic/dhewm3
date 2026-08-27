/*
===========================================================================

dhewm 3 on eacp - the application object Apps::run<T>() owns.

===========================================================================
*/

#ifndef __SYS_EACP_APP_H__
#define __SYS_EACP_APP_H__

#include "View.h"

#include "../../framework/Licensee.h"

#include <eacp/Graphics/Window/Window.h>

namespace dhewm3
{
// r_mode 5, dhewm3's default, and the size R_InitOpenGL will ask for once the
// engine is the one opening the window. A fixed number rather than a share of
// the display, because it is the engine's cvar that owns this and the window
// is about to start being sized from it.
constexpr auto defaultWindowWidth = 1024;
constexpr auto defaultWindowHeight = 768;

inline Graphics::WindowOptions windowOptions()
{
	auto options = Graphics::WindowOptions {};

	options.width = defaultWindowWidth;
	options.height = defaultWindowHeight;
	options.title = ENGINE_VERSION;

	return options;
}

struct App
{
	App()
	{
		window.setContentView(view);
		view.focus();
	}

	Graphics::Window window {windowOptions()};
	View view;
};
} // namespace dhewm3

#endif /* !__SYS_EACP_APP_H__ */
