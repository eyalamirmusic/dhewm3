/*
===========================================================================

dhewm 3 on eacp - what is left of GLimp.

GLimp is OpenGL's window and context: creating one, making it current, swapping
it, and setting the gamma ramp on the display it is on. Here the window is
eacp's and there is no context at all, so nearly all of this is either a
statement about what the host is already doing (the two functions the frame
calls) or an entry point nobody on this build can reach.

What is real is GLimp_Init, and it does not initialize anything: it reports the
size of the surface the view already has. That is the one thing R_InitOpenGL
genuinely needs from the platform, and it is the reason this file did not go
away entirely when idRenderBackendEacp landed.

===========================================================================
*/

// eacp first: idlib/Str.h turns strcmp and friends into macros, which breaks
// any standard header included after it. See RenderBackend_Eacp.cpp.
#include <eacp/GPU/GPU.h>

#include "sys/platform.h"
#include "framework/Common.h"
#include "renderer/tr_local.h"
#include "renderer/RenderBackend_Eacp.h"
#include "sys/sys_imgui.h"

static void GLimp_Unreachable( const char *what ) {
	// Not a warning. Every one of these is either OpenGL's own or a window
	// feature eacp has not grown yet (plan.md section 5, gap 8), and reaching
	// one means something asked for a capability this build said it did not
	// have.
	common->FatalError( "%s called on the eacp build, which has no OpenGL", what );
}

/*
===================
GLimp_Init

Not an initialization. The window exists before the engine does - it is what
the engine is started from (sys/eacp/View.cpp) - so all this does is measure it.

The two sizes are different and both matter. vidWidth/vidHeight are real pixels,
which is what the renderer rasterizes into and what a scissor rect is in;
winWidth/winHeight are logical points, which is what the window is laid out in
and what a mouse coordinate arrives in. On a Retina display they differ by two,
and dhewm3 grew the distinction for exactly this reason.

r_mode is therefore not honoured, and neither is r_fullscreen: the window was
sized before the cvars were read. Resizing it from here is gap 8's other half
and is worth doing once the picture is worth looking at.
===================
*/
bool GLimp_Init( glimpParms_t parms ) {
	eacp::GPU::GPUView *	view = R_EacpGetView();

	if ( !view ) {
		common->FatalError( "GLimp_Init: no eacp view - the engine was started "
		                    "from somewhere other than the view" );
		return false;
	}

	const float	scale = view->backingScale();
	const auto	bounds = view->getLocalBounds();

	glConfig.winWidth = bounds.w;
	glConfig.winHeight = bounds.h;

	glConfig.vidWidth = (int)( bounds.w * scale );
	glConfig.vidHeight = (int)( bounds.h * scale );

	glConfig.isFullscreen = false;
	glConfig.displayFrequency = (int)GLimp_GetDisplayRefresh();

	common->Printf( "eacp view: %dx%d pixels (%.0fx%.0f points at %.2gx)\n",
					glConfig.vidWidth, glConfig.vidHeight,
					glConfig.winWidth, glConfig.winHeight, scale );

	// The settings menu, which the SDL host also brought up from here - at the
	// end of glimp.cpp's own GLimp_Init, once there was a window and a context
	// to draw into. Here there is a window and the two sizes just measured,
	// which is what the menu's scale and its projection are measured against;
	// the render backend comes up after this and neither half of ImGui touches
	// the device before its first draw.
	//
	// A failure is not fatal, and is not treated as one anywhere: Init warns and
	// leaves imguiCtx NULL, which is the thing every entry point in
	// sys/sys_imgui.cpp holds as "no menu".
	D3::ImGuiHooks::Init();

	return true;
}

bool GLimp_SetScreenParms( glimpParms_t parms ) {
	// r_mode, r_fullscreen and vid_restart. The window is the host's and this
	// build cannot resize it (gap 8), so refusing is honest - R_SetScreenParms
	// treats a false as "the mode did not change" rather than as an error.
	return false;
}

void GLimp_Shutdown() {
	// Reached through idRenderSystemLocal::ShutdownOpenGL, which the shutdown
	// path calls whether or not anything was ever started.
	//
	// The settings menu is the one thing there is to undo, and this is where the
	// SDL host undid it too. The order matters and is already right: that
	// function runs the render backend down first, which is what lets go of
	// ImGui's textures while the context they are named in still exists.
	D3::ImGuiHooks::Shutdown();
}

void GLimp_SwapBuffers() {
	GLimp_Unreachable( "GLimp_SwapBuffers" );
}

void GLimp_SetGamma( unsigned short red[256], unsigned short green[256], unsigned short blue[256] ) {
	// The display's hardware ramp, which eacp does not expose and which
	// dhewm3 stopped needing: r_gammaInShader defaults to 1 and applies
	// r_gamma and r_brightness in the shader instead. Warned rather than
	// fatal, because the only way here is a player turning that cvar off.
	static bool	warned = false;

	if ( !warned ) {
		warned = true;
		common->Warning( "r_gammaInShader 0 asks for the display's hardware gamma ramp, "
		                 "which this build cannot set - leave it at 1" );
	}
}

void GLimp_ResetGamma() {
	// Same as GLimp_Shutdown: called on the way out regardless.
}

void GLimp_ActivateContext() {
}

void GLimp_DeactivateContext() {
}

// GLimp_GrabInput is in sys/eacp/Input.cpp, and it belongs there: it is the
// input layer's, and the only reason it sat in sys/glimp.cpp in the SDL build
// was that SDL's grab calls need the window handle that file owned. Here the
// window belongs to the view, which the input layer already tracks.

bool GLimp_SetSwapInterval( int swapInterval ) {
	return false;
}

/*
================
GLimp_GetSwapInterval / GLimp_GetDisplayRefresh

The two that are called every frame, from the tail of idCommonLocal::Frame,
and the reason they answer 1 and 60 rather than 0 and 0.

That tail decides whether to sleep until the next 60Hz tic boundary. Under
vsync at 60Hz it does not, because the swap already blocked for exactly that
long, and sleeping on top of it would let the engine's clock and the display
drift apart. Here the display link is the swap: GPUView::update is called once
per refresh and the frame is paced by the panel, so a sleep inside it would be
sleeping inside the callback that the next refresh is waiting on.

So this is not a lie about hardware that isn't there - it is the same statement
the SDL build makes when vsync is on, said by the host that is doing the
pacing. The view caps itself at 60fps to make the other half of it true on a
120Hz panel (View.cpp).
================
*/
int GLimp_GetSwapInterval() {
	return 1;
}

float GLimp_GetDisplayRefresh() {
	return 60.0f;
}

bool GLimp_SetWindowResizable( bool enableResizable ) {
	return false;
}

void GLimp_UpdateWindowSize() {
}

glimpParms_t GLimp_GetCurState() {
	glimpParms_t ret = {};

	ret.width = glConfig.vidWidth;
	ret.height = glConfig.vidHeight;
	ret.fullScreen = false;
	ret.fullScreenDesktop = false;
	ret.stereo = false;
	ret.displayHz = 60;
	ret.multiSamples = 0;

	return ret;
}
