#include "View.h"

#include "Input.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Graphics/Window/Window.h>
#include <eacp/Sprites/SpriteRenderer.h>
#include <eacp/Text/TextRenderer.h>

#include <algorithm>
#include <cstdarg>
#include <string>
#include <vector>

#include "sys/platform.h"
#include "framework/Common.h"
#include "renderer/tr_local.h" // glConfig, GLimp_UpdateWindowSize
#include "renderer/RenderBackend_Eacp.h"

namespace dhewm3
{
Graphics::Rect contentRect()
{
    auto* view = R_EacpGetView();

    if (view == nullptr)
        return {};

    const auto bounds = view->getLocalBounds();

    // Clamped rather than trusted, because the two are written by different
    // things: the bounds are the window's and change the moment the user drags
    // a corner, while winWidth/winHeight are last frame's fit. A stale fit that
    // is a few points too big would put the picture's edge outside the window
    // and every mouse point with it.
    const auto w = std::min((float) glConfig.winWidth, bounds.w);
    const auto h = std::min((float) glConfig.winHeight, bounds.h);

    if (w <= 0.f || h <= 0.f)
        return bounds;

    return {(bounds.w - w) * 0.5f, (bounds.h - h) * 0.5f, w, h};
}

namespace
{
std::string formatted(const char* format, ...)
{
    char buffer[512] = {};

    // idStr's, because idlib/Str.h turns vsnprintf into a compile error.
    va_list args;
    va_start(args, format);
    idStr::vsnPrintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    return buffer;
}

std::string megabytes(std::int64_t bytes)
{
    return formatted("%.1f MB", (double) bytes / (1024.0 * 1024.0));
}
} // namespace

// Two renderers and nothing else. Built on the first refresh that has a download
// to show rather than with the view, because building them compiles pipelines
// and loads a font - a second's work a machine that has game data never needs.
struct View::DemoScreen
{
    explicit DemoScreen(int sampleCount)
        : sprites(Graphics::Point {1.0f, 1.0f}, sampleCount)
    {
    }

    Sprites::SpriteRenderer sprites;
    Text::TextRenderer text {14.0f};
};

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

    // One sample, which is a consequence rather than a preference, and stays one
    // now that r_multiSamples means something again.
    //
    // Since step 4e the frame is composed into an app-owned render target and
    // the drawable is a blit of it. Multisampling belongs to the target, where
    // the scene is rasterized (TextureDescriptor::sampleCount, eacp gap 20); by
    // the time the blit runs, the picture has already been resolved, and asking
    // the drawable for four samples would only make one full-screen quad
    // rasterize four times to produce the same pixels.
    //
    // What the view did *not* have before step 4e was any say at all - it was on
    // GPUView's default of four, and the renderer had no way to reach it.
    setSampleCount(1);

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

    // A view is not sent mouse events unless it says it wants them, and it does
    // not take key focus on a click unless it says that either. Both are the
    // defaults a widget wants and neither is what a game wants.
    setHandlesMouseEvents(true);
    setGrabsFocusOnMouseDown(true);

    // The grab (Input.h, GLimp_GrabInput) needs a Window to lock the mouse, and
    // this is what it finds one through.
    Input::setView(this);

    // And the renderer needs it for the one thing GLimp still answers: how big
    // the surface it is drawing into actually is.
    R_EacpSetView(this);
}

View::~View()
{
    Input::setView(nullptr);
    R_EacpSetView(nullptr);

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
    //
    // What DemoData has to say goes first: fs_cdpath, when the engine is to run
    // on the demo it downloaded.
    auto arguments = demoData->engineArguments();
    const auto& commandLine = Apps::getAppEnvironment().commandLineArgs;

    for (std::size_t i = 1; i < commandLine.size(); ++i)
        arguments.push_back(commandLine[i]);

    auto argv = std::vector<char*> {};

    for (auto& argument: arguments)
        argv.push_back(argument.data());

    common->Init((int) argv.size(), argv.data());
}

void View::update(Threads::FrameTime)
{
    // Where the picture goes in the window, re-measured. The window cannot be
    // resized to r_mode, but the user can resize it to anything, and a drag of
    // the corner is the one thing that moves the fit GLimp_UpdateWindowSize
    // computed - so it is recomputed here rather than waited for, there being no
    // resize the engine would otherwise hear about.
    //
    // In update() rather than render() because glConfig.vidWidth/vidHeight are
    // borrowed for the length of a frame (a tiled screenshot renders into them),
    // and this is the side of common->Frame() where they are the mode's.
    GLimp_UpdateWindowSize();

    // Shift, Ctrl and Alt arrive as state and never as key events (plan.md §5,
    // gap 9), so they are polled once a frame and the difference is turned into
    // the down/up pair the engine binds - _attack, _strafe and _speed. Read
    // from the window rather than globally so a modifier held while another app
    // is in front is not the player's.
    if (auto* host = getWindow())
        Input::syncModifiers(host->getModifiers());
}

void View::render(GPU::Frame& frame)
{
    // The engine's frame *is* a frame, which is why this is not in update().
    //
    // idCommonLocal::Frame ends by issuing the render commands it built, and
    // the backend consumes them right there - inside the call, not after it. So
    // the eacp Frame has to be open around the whole of common->Frame(), and
    // update() is the wrong side of that: eacp hands a Frame to render() and to
    // nothing else.
    //
    // What the engine sees is unchanged. Doom 3's own host ran this in a
    // `while (1)` and idCommonLocal::Frame sleeps at the end of it to hold
    // 60Hz; here the display link is the thing that waits, and the engine is
    // told not to sleep on top of it (sys/eacp/GLimp.cpp).
    R_EacpSetFrame(&frame);

    // Not started until there is game data to start it with. On a machine that
    // has some that is the first refresh, as it always was; on one that has none
    // it is once the demo is downloaded and unpacked, with the window showing
    // how far along that is in the meantime.
    if (!engineStarted)
    {
        if (!demoData)
            demoData.emplace();

        demoData->update();
    }

    if (engineStarted)
        common->Frame();
    else if (demoData->stage() == DemoData::Stage::ready)
        startEngine();
    else
        drawDemoScreen(frame);

    // Null outside the frame, so that a draw issued from anywhere else - a
    // console command, a level load's own screen update - is a no-op that says
    // so rather than a use of a Frame that has already presented.
    R_EacpSetFrame(nullptr);
}

void View::drawDemoScreen(GPU::Frame& frame)
{
    if (!demoScreen)
        demoScreen = std::make_unique<DemoScreen>(sampleCount());

    auto& sprites = demoScreen->sprites;
    auto& text = demoScreen->text;
    const auto& data = *demoData;

    const auto bounds = getLocalBounds();
    auto pass = frame.beginPass({Graphics::Color {0.06f, 0.06f, 0.07f}});

    sprites.setLogicalSize({bounds.w, bounds.h});
    sprites.begin(pass);

    text.setViewport({bounds.w, bounds.h}, frame.backingScale());
    text.begin();

    const auto bright = Graphics::Color {0.92f, 0.92f, 0.92f};
    const auto dim = Graphics::Color {0.55f, 0.55f, 0.58f};
    const auto red = Graphics::Color {1.0f, 0.45f, 0.4f};

    const auto width = std::min(bounds.w - 64.0f, 640.0f);
    const auto left = (bounds.w - width) * 0.5f;
    const auto line = text.lineHeight();
    const auto bar = Graphics::Rect {left, bounds.h * 0.5f - 10.0f, width, 20.0f};

    // Two lines, because one is wider than the bar and a narrow window cuts it.
    const auto drawHint = [&](float y)
    {
        text.draw("To play the full game instead, quit and start dhewm3 with", {left, y}, dim);
        text.draw("+set fs_basepath <your Doom 3 directory>", {left, y + line}, dim);
    };

    if (data.stage() == DemoData::Stage::failed)
    {
        text.draw("The Doom 3 demo could not be fetched", {left, bar.y}, bright);
        text.draw(data.error(), {left, bar.y + line * 1.5f}, red);
        text.draw("Click anywhere to try again.", {left, bar.y + line * 3.0f}, dim);
        drawHint(bar.y + line * 4.5f);
    }
    else
    {
        const auto done = data.bytesDone();
        const auto total = data.bytesTotal();
        const auto fraction = total > 0 ? (float) ((double) done / (double) total) : -1.0f;
        const auto unpacking = data.stage() == DemoData::Stage::unpacking;

        const auto heading = unpacking
                                 ? "Unpacking demo00.pk4 out of the demo installer"
                                 : "No Doom 3 game data was found, so dhewm3 is "
                                   "downloading the free demo";

        // A server that declares no length gives no percentage to show, only
        // how much has arrived.
        const auto status =
            unpacking ? formatted("%.0f%%", std::max(0.0f, fraction) * 100.0f)
            : fraction >= 0.0f
                ? formatted("%s of %s  (%.0f%%)",
                            megabytes(done).c_str(),
                            megabytes(total).c_str(),
                            fraction * 100.0f)
                : formatted("%s so far", megabytes(done).c_str());

        sprites.fillRect(bar, {1.0f, 1.0f, 1.0f, 0.08f});

        auto filled = bar;
        filled.w = bar.w * std::max(0.0f, fraction);
        sprites.fillRect(filled, {0.72f, 0.16f, 0.1f, 1.0f});
        sprites.drawRect(bar, {1.0f, 1.0f, 1.0f, 0.25f}, 1.0f);

        text.draw(heading, {left, bar.y - line * 0.8f}, bright);
        text.draw(status, {left, bar.y + bar.h + line * 1.2f}, dim);
        drawHint(bar.y + bar.h + line * 3.2f);
    }

    // The quads first, so the text lands on top of the bar: the sprite queue
    // is otherwise drawn when the pass ends, which is after the glyphs.
    sprites.flush();
    text.flush(pass);
}

/*
    Input. Every one of these is a forwarder: what a key means is Doom 3's
    question and Input.cpp answers it, and a view that decided anything here
    would be a second place to look when a binding does not work.
*/

void View::keyDown(const Graphics::KeyEvent& event)
{
    // Auto-repeat is passed through rather than filtered. The engine wants it:
    // holding backspace in the console has to keep deleting, and dhewm3 reads
    // that repeat out of the same SE_KEY stream SDL hands it.
    Input::keyEvent(event, true);
}

void View::keyUp(const Graphics::KeyEvent& event)
{
    Input::keyEvent(event, false);
}

void View::mouseDown(const Graphics::MouseEvent& event)
{
    // Before the engine is started the only thing in the window is the demo
    // screen, and the one thing a click does there is try a failed fetch again.
    if (!engineStarted)
    {
        if (demoData)
            demoData->retry();

        return;
    }

    Input::mouseButton(event, true);
}

void View::mouseUp(const Graphics::MouseEvent& event)
{
    Input::mouseButton(event, false);
}

void View::mouseMoved(const Graphics::MouseEvent& event)
{
    Input::mouseMotion(event);
}

void View::mouseDragged(const Graphics::MouseEvent& event)
{
    // A drag is a move with a button held, and Doom 3 draws no distinction:
    // aiming while firing is the ordinary case.
    Input::mouseMotion(event);
}

void View::mouseExited(const Graphics::MouseEvent&)
{
    // Doom 3 has nothing to do with this - the pointer leaving the window says
    // nothing about a player's aim - and the settings menu does: a widget the
    // cursor was over stays highlighted for the rest of the run otherwise.
    Input::mouseExited();
}

void View::mouseWheel(const Graphics::MouseEvent& event)
{
    Input::mouseWheel(event);
}
} // namespace dhewm3
