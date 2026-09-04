/*
===========================================================================

dhewm 3 on eacp - what is left of GLimp.

GLimp is OpenGL's window and context: creating one, making it current, swapping
it, and setting the gamma ramp on the display it is on. Here the window is
eacp's and there is no context at all, so nearly all of this is either a
statement about what the host is already doing (the two functions the frame
calls) or an entry point nobody on this build can reach.

What is real is GLimp_Init and GLimp_UpdateWindowSize, and between them they do
the one thing R_InitOpenGL genuinely needs from the platform: decide what size
the renderer rasterizes at, and what size the window shows it at. Those are two
different numbers here in a way they are not on the SDL host, and the comment on
GLimp_UpdateWindowSize is where that is explained.

===========================================================================
*/

// eacp first: idlib/Str.h turns strcmp and friends into macros, which breaks
// any standard header included after it. See RenderBackend_Eacp.cpp.
#include <eacp/GPU/GPU.h>

// And this one before Doom 3's for the same reason - it is deliberately free of
// them, see its own header comment. R_EacpFrameSampleCount is what
// GLimp_GetCurState answers the settings menu with.
#include "renderer/RenderProgs_Eacp.h"

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
The mode, in pixels - which is the whole of what r_mode means on this host.

It is what the frame's render target is created at
(idRenderBackendEacp::EnsureFrameTarget, which sizes itself from
glConfig.vidWidth) and what the window's picture is fitted to.

Kept here rather than read back out of glConfig.vidWidth, even though GLimp_Init
is what put it there, because those two are *borrowed* for the length of a
frame: idRenderSystemLocal::BeginFrame points them at the tile a
larger-than-window screenshot is being rendered into (com_aviDemoWidth, the
regression gate's capture path) and at r_screenFraction's crop, and puts them
back in EndFrame. The window's letterbox is not theirs to move.
===================
*/
static int	glimpModeWidth = 0;
static int	glimpModeHeight = 0;

/*
===================
GLimp_WarnFullscreen

r_fullscreen, once.

eacp's Graphics::Window takes its flags - WindowFlags::FullScreen,
WindowFlags::Borderless - and its size at construction and has no setter for
either, so neither real fullscreen nor the borderless display-sized window an
app would build one out of can be entered from here (plan.md §5, gap 8).

**Warned rather than refused, in both places that ask.** Refusing sends
R_InitOpenGL round its retry loop, which drops r_mode to 3 as well and loses the
resolution over a window flag; and refusing in GLimp_SetScreenParms falls back to
a full vid_restart that purges every image and *still* cannot make the window
fullscreen. A windowed game is the honest outcome either way, so it is reached
the cheap way and said out loud once.
===================
*/
static void GLimp_WarnFullscreen( void ) {
	static bool	warned = false;

	if ( warned ) {
		return;
	}

	warned = true;

	common->Warning( "r_fullscreen asks for a fullscreen window, which this build "
	                 "cannot make: eacp's window takes its size and its flags at "
	                 "construction and has no setter for either. Staying windowed" );
}

/*
===================
GLimp_ReportMode

The one line that says what the mode turned into, printed from the two places
that can change it. Everything in it is measured rather than intended, which is
the point: r_mode asks for a size, and this says what the renderer and the
window actually ended up as.
===================
*/
static void GLimp_ReportMode( void ) {
	eacp::GPU::GPUView *	view = R_EacpGetView();
	const auto				bounds = view ? view->getLocalBounds() : eacp::Graphics::Rect();
	const float				scale = view ? view->backingScale() : 1.0f;

	common->Printf( "eacp view: %dx%d pixels (r_mode %d), drawn into %gx%g of a "
					"%gx%g-point window at %.3gx\n",
					glimpModeWidth, glimpModeHeight, r_mode.GetInteger(),
					glConfig.winWidth, glConfig.winHeight,
					bounds.w, bounds.h, scale );
}

/*
===================
GLimp_Init

Not an initialization: the window exists before the engine does - it is what the
engine is started from (sys/eacp/View.cpp) - so there is nothing here to create.
What this does is settle the two sizes the rest of the engine measures against.

**vidWidth/vidHeight are r_mode's, in pixels**, which is what the SDL host on a
1x display gives you and is what the renderer rasterizes into: every viewport,
scissor and screen coordinate in the renderer is in them, and since step 4e the
frame is composed into a render target created at exactly those two. Taking them
from parms - R_InitOpenGL has already run R_GetModeInfo, so r_mode -1's
r_customWidth/r_customHeight are in there too - is therefore the whole of
honouring r_mode.

It is also the fix for what this used to do, which was to measure the *drawable*
and rasterize at that: on a 2x display a 1024x768-point window meant a 2048x1536
picture, four times the pixels r_mode 5 asked for, paid for on every fill-rate
bound surface in the game.

**winWidth/winHeight are points**, which is what the window is laid out in and
what a mouse coordinate arrives in - see GLimp_UpdateWindowSize, which is where
the difference between this host and the SDL one lives.
===================
*/
bool GLimp_Init( glimpParms_t parms ) {
	eacp::GPU::GPUView *	view = R_EacpGetView();

	if ( !view ) {
		common->FatalError( "GLimp_Init: no eacp view - the engine was started "
		                    "from somewhere other than the view" );
		return false;
	}

	if ( parms.width < 1 || parms.height < 1 ) {
		// R_InitOpenGL's second try, which resets r_mode to 3 and comes back.
		common->Warning( "r_mode %d asks for a %dx%d picture, which is not a size",
						 r_mode.GetInteger(), parms.width, parms.height );
		return false;
	}

	if ( parms.fullScreen ) {
		GLimp_WarnFullscreen();
	}

	glimpModeWidth = parms.width;
	glimpModeHeight = parms.height;

	glConfig.vidWidth = glimpModeWidth;
	glConfig.vidHeight = glimpModeHeight;

	glConfig.isFullscreen = false;
	glConfig.displayFrequency = (int)GLimp_GetDisplayRefresh();

	GLimp_UpdateWindowSize();
	GLimp_ReportMode();

	// The settings menu, which the SDL host also brought up from here - at the
	// end of glimp.cpp's own GLimp_Init, once there was a window and a context
	// to draw into. Here there is a window and the two sizes just settled,
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

/*
===================
GLimp_UpdateWindowSize

winWidth/winHeight: the part of the window the frame is drawn into, in points.

**Not the window's size, and that is the one place this host has to differ from
the SDL one.** There the window is created at r_mode's size and later resized to
it, so the picture and the window are the same rectangle. eacp's
Graphics::Window has no size setter at all - width and height are
WindowOptions fields read once at construction (plan.md §5, gap 8) - so here the
render target and the window are free to be different shapes, and the frame is
*fitted* into the window rather than stretched across it: the largest rectangle
of the target's aspect that fits, centred, with black either side.
idRenderBackendEacp::PresentFrameTarget draws it there, deriving the same
rectangle from these two numbers and the view's bounds.

**Reporting the picture rather than the window is what keeps the point-space
mappings honest.** winWidth/winHeight are what the menu cursor's speed is scaled
by (idUserInterface::HandleEvent), what ImGui lays itself out in and what a
mouse position is measured against (sys/eacp/ImGuiBackend.cpp) - and all three
of those are about the thing the player is pointing at, which is the picture.
sys/eacp/Input.cpp and ImGuiBackend.cpp shift an incoming point by the letterbox
offset for the same reason.

Called every frame from View::update, because a window this host cannot resize
is still one the *user* can: WindowFlags::Resizable is on by default, and a drag
of the corner changes what the picture has to be fitted into.
===================
*/
void GLimp_UpdateWindowSize() {
	eacp::GPU::GPUView *	view = R_EacpGetView();

	if ( !view ) {
		return;
	}

	const auto	bounds = view->getLocalBounds();

	if ( bounds.w < 1.0f || bounds.h < 1.0f ) {
		return;
	}

	if ( glimpModeWidth < 1 || glimpModeHeight < 1 ) {
		// Before GLimp_Init has read a mode there is no picture to fit, so the
		// window is the picture. Reached every frame between the window opening
		// and common->Init getting as far as R_InitOpenGL.
		glConfig.winWidth = bounds.w;
		glConfig.winHeight = bounds.h;
		return;
	}

	float		fit = bounds.w / (float)glimpModeWidth;
	const float	fitHeight = bounds.h / (float)glimpModeHeight;

	if ( fitHeight < fit ) {
		fit = fitHeight;
	}

	// Whole points. The fit is rarely exact - 1234x567 in a 1024x768 window is
	// 470.509 points tall - and a fractional one buys nothing: winWidth/winHeight
	// are printed as a size and compared against integers by the settings menu,
	// and no consumer of them can express half a point anyway. What matters is
	// that the blit and the mouse read the same two numbers, which they do, so
	// rounding costs at most a point of aspect out of 768 and never puts the
	// cursor and the picture out of step with each other.
	glConfig.winWidth = idMath::Floor( (float)glimpModeWidth * fit );
	glConfig.winHeight = idMath::Floor( (float)glimpModeHeight * fit );
}

/*
===================
GLimp_SetScreenParms

'vid_restart partial', which is what the settings menu's Apply runs and what
R_VidRestart_f tries before tearing the renderer down. Answering false is not an
error - it falls back to the full restart, which comes back through GLimp_Init.

**A resolution change really is nothing here**, which is what step 4e bought:
the frame is composed into a render target sized from glConfig.vidWidth, so
moving those two is the whole of the change and the next EnsureFrameTarget
builds the target it does not have. This used to return false unconditionally,
which meant every mode change paid for a full restart - images purged and
reloaded, interactions regenerated - to do exactly this.

The one thing it cannot do is the pipelines': multisampling is compiled into
every one of them and only idRenderBackendEacp::Init drops the ones built
against the old count, so a changed r_multiSamples falls through to the full
restart. Which is what the SDL host does with it too, and for the same reason.

r_fullscreen is warned about rather than refused - GLimp_WarnFullscreen says why.
===================
*/
bool GLimp_SetScreenParms( glimpParms_t parms ) {
	const glimpParms_t	current = GLimp_GetCurState();

	// -1 is R_VidRestart_f's "keep whatever is active" - 'vid_restart partial
	// windowed', the path an error takes on its way back to a window.
	if ( parms.multiSamples != -1 && parms.multiSamples != current.multiSamples ) {
		return false;
	}

	if ( parms.fullScreen ) {
		GLimp_WarnFullscreen();
	}

	if ( parms.width < 1 || parms.height < 1 ) {
		return false;
	}

	if ( parms.width == glimpModeWidth && parms.height == glimpModeHeight ) {
		return true;
	}

	glimpModeWidth = parms.width;
	glimpModeHeight = parms.height;

	glConfig.vidWidth = glimpModeWidth;
	glConfig.vidHeight = glimpModeHeight;

	GLimp_UpdateWindowSize();
	GLimp_ReportMode();

	return true;
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
	// r_windowResizable. WindowFlags::Resizable is one of the four eacp puts on
	// a window by default, so this build's window *is* resizable - what it has
	// no way to do is change its mind, the flags being read once at
	// construction. Refusing is honest; the cvar's description says so too.
	return false;
}

/*
===================
GLimp_GetCurState

What the settings menu compares the cvars against to decide whether its Apply
button has anything to do (Dhewm3SettingsMenu.cpp, VideoHasApplyableChanges),
and what GLimp_SetScreenParms above reads its own multisampling out of.

The size is the mode's rather than glConfig's, for the reason glimpModeWidth
exists at all; and the sample count is in the cvar's own terms, where 0 and 1
both mean no multisampling - the menu writes 0 and would otherwise see an
applyable change that was never made.
===================
*/
glimpParms_t GLimp_GetCurState() {
	glimpParms_t	ret = {};
	const int		samples = R_EacpFrameSampleCount();

	ret.width = glimpModeWidth;
	ret.height = glimpModeHeight;
	ret.fullScreen = false;
	ret.fullScreenDesktop = false;
	ret.stereo = false;
	ret.displayHz = (int)GLimp_GetDisplayRefresh();
	ret.multiSamples = ( samples > 1 ) ? samples : 0;

	return ret;
}
