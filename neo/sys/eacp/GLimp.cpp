/*
===========================================================================

dhewm 3 on eacp - GLimp, stubbed.

The renderer is the last thing to come up (plan.md, Phase 2 step 4), so this
build runs with com_skipRenderer 1 and R_InitOpenGL is never reached: nothing
here opens a window or makes a context, and the two functions with real bodies
are the two idCommonLocal::Frame calls every frame.

The stubs stay until idRenderBackendEacp lands, at which point GLimp goes away
rather than gets an implementation - a window is eacp's, and the seam the
renderer will be behind is idRenderBackend, not this.

===========================================================================
*/

#include "sys/platform.h"
#include "framework/Common.h"
#include "renderer/tr_local.h"

static void GLimp_Unreachable( const char *what ) {
	// Not a warning: reaching any of these means the renderer came up, and the
	// renderer coming up on this build means com_skipRenderer was turned off
	// on a host that has no GL context to turn it on for.
	common->FatalError( "%s called on the eacp build, which has no OpenGL "
	                    "(run with com_skipRenderer 1 until Phase 2 step 4)", what );
}

bool GLimp_Init( glimpParms_t parms ) {
	GLimp_Unreachable( "GLimp_Init" );
	return false;
}

bool GLimp_SetScreenParms( glimpParms_t parms ) {
	GLimp_Unreachable( "GLimp_SetScreenParms" );
	return false;
}

void GLimp_Shutdown() {
	// Reached through idRenderSystemLocal::ShutdownOpenGL, which the shutdown
	// path calls whether or not anything was ever started. Nothing to undo.
}

void GLimp_SwapBuffers() {
	GLimp_Unreachable( "GLimp_SwapBuffers" );
}

void GLimp_SetGamma( unsigned short red[256], unsigned short green[256], unsigned short blue[256] ) {
	GLimp_Unreachable( "GLimp_SetGamma" );
}

void GLimp_ResetGamma() {
	// Same as GLimp_Shutdown: called on the way out regardless.
}

void GLimp_ActivateContext() {
}

void GLimp_DeactivateContext() {
}

GLExtension_t GLimp_ExtensionPointer( const char *name ) {
	return NULL;
}

void GLimp_GrabInput( int flags ) {
	// Mouse capture is Window::setMouseLocked here, driven from the view.
	// Phase 2 step 3.
}

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
