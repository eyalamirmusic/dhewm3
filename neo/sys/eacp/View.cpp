#include "View.h"

#include <eacp/Core/App/AppEnvironment.h>

#include <string>
#include <vector>

#include "sys/platform.h"
#include "framework/Common.h"

namespace dhewm3
{
View::View()
{
	// Doom 3's lighting is stencil shadow volumes, so the drawable needs a
	// stencil plane before any of it can be drawn. Asked for here rather than
	// with the renderer because it sizes the view's own attachments: it is a
	// property of the surface being drawn into, not of a pipeline drawing to
	// it. It implies depth - both APIs carry the two planes in one attachment
	// of one combined format, so a view asking for stencil allocates the depth
	// plane whether or not anything tests it.
	setStencil(true);

	// The engine's frame is driven off the display link rather than on demand:
	// a game redraws every refresh whether or not anything the platform layer
	// can see has changed.
	setContinuous(true);

	// Doom 3's clock runs at 60Hz - USERCMD_HZ, the rate com_ticNumber
	// advances at - and nothing between tics moves, so a 120Hz panel would
	// draw every state twice. Capping here is also the other half of what
	// GLimp_GetSwapInterval claims: the engine skips its own sleep because the
	// display link is what paces it, and this is what makes that pacing 60.
	setMaxFps(60);
}

View::~View()
{
	// Reached twice over, and only one of them wants a shutdown.
	//
	// Once when the loop unwinds - the window closed, or Cmd+Q - and the engine
	// is still up. And once when the engine quit itself: idCommonLocal::Quit
	// shuts down and then calls Sys_Quit, which exits the process from inside
	// the frame, and exit() destroys the app that owns this view. Shutting an
	// already shut-down engine down again dies in the file system.
	if (common->IsInitialized())
		common->Shutdown();
}

void View::startEngine()
{
	engineStarted = true;

	// Apps::run snapshotted main()'s argv. Index 0 is the executable path,
	// which idCommonLocal::ParseCommandLine says it does not want.
	auto arguments = std::vector<std::string> {};
	const auto& commandLine = Apps::getAppEnvironment().commandLineArgs;

	for (std::size_t i = 1; i < commandLine.size(); ++i)
		arguments.push_back(commandLine[i]);

	// The renderer is Phase 2 step 4, and until it exists this build has no GL
	// context for R_InitOpenGL to want: com_skipRenderer is what stops it being
	// asked for. sys/eacp/GLimp.cpp is the other half - every entry point that
	// would need one is a fatal error naming this line.
	//
	// Appended rather than prepended, and the difference is not style:
	// ParseCommandLine starts a new console line at each argument beginning
	// with '+' and appends everything else to the line before it, so a leading
	// `+set com_skipRenderer 1` would swallow a user argument that does not
	// start with '+' into its own value list. At the end nothing can follow it.
	arguments.push_back("+set");
	arguments.push_back("com_skipRenderer");
	arguments.push_back("1");

	auto argv = std::vector<char*> {};

	for (auto& argument : arguments)
		argv.push_back(argument.data());

	common->Init((int) argv.size(), argv.data());
}

void View::update(Threads::FrameTime)
{
	if (!engineStarted)
		startEngine();

	// The whole of dhewm3's main loop. sys/linux/main.cpp runs this in a
	// `while (1)` and idCommonLocal::Frame sleeps at the end of it to hold 60Hz;
	// here the display link is the thing that waits, and the engine is told not
	// to sleep on top of it (sys/eacp/GLimp.cpp).
	common->Frame();
}

void View::render(GPU::Frame& frame)
{
	// Nothing to draw yet - the engine is running headless until step 4 - but
	// the pass still has to be opened and closed: a frame that begins no pass
	// presents whatever the drawable happened to contain, which on a freshly
	// allocated one is undefined.
	frame.beginPass({Graphics::Color {0.05f, 0.05f, 0.06f}});
}
} // namespace dhewm3
