/*
===========================================================================

dhewm 3 on eacp - the window's contents, and the engine's host.

Phase 2 of ../../../plan.md. The engine is started from here rather than from
main() because the renderer will want a window to come up in (step 4), and
update() is the first place there certainly is one: the view is in the window,
sized, and its Metal layer exists.

It is also where the platform's input arrives. eacp delivers the keyboard and
the mouse as callbacks on the view, and every one of them is forwarded to
Input.h - the queue the engine polls - rather than acted on here. The two jobs
are kept apart on purpose: this file hosts the engine, and that one is the
platform's input, which happens to be delivered to a view.

Today the engine runs headless - com_skipRenderer 1, see View.cpp - so what is
on screen is still a cleared window. What is different is that behind it the
file system, the sound system, the game library and the session are all up,
common->Frame() is running once a refresh, and it is being driven by a keyboard.

===========================================================================
*/

#pragma once

#include <eacp/GPU/GPU.h>

namespace dhewm3
{
using namespace eacp;

// The part of the window the frame is drawn into, in points.
//
// The window cannot be resized to r_mode - eacp's Graphics::Window has no size
// setter (plan.md §5, gap 8) - so the picture is fitted into whatever window
// there is rather than stretched across it, and this is where it lands: the
// largest rectangle of the render target's aspect, centred, with black either
// side. GLimp_UpdateWindowSize (sys/eacp/GLimp.cpp) is what decides its size and
// keeps it in glConfig.winWidth/winHeight; this places that size in the window.
//
// Which is what an incoming mouse point has to be measured from, because
// everything that reads winWidth/winHeight - the menu cursor, ImGui's layout -
// is measuring the picture and not the window.
Graphics::Rect contentRect();

struct View final : GPU::GPUView
{
    View();
    ~View() override;

    void update(Threads::FrameTime) override;
    void render(GPU::Frame& frame) override;

    void keyDown(const Graphics::KeyEvent& event) override;
    void keyUp(const Graphics::KeyEvent& event) override;

    void mouseDown(const Graphics::MouseEvent& event) override;
    void mouseUp(const Graphics::MouseEvent& event) override;
    void mouseMoved(const Graphics::MouseEvent& event) override;
    void mouseDragged(const Graphics::MouseEvent& event) override;
    void mouseExited(const Graphics::MouseEvent& event) override;
    void mouseWheel(const Graphics::MouseEvent& event) override;

private:
    // common->Init, once, on the first refresh. It reads pk4s and loads the
    // game library, so it takes a second or two of that refresh; the display
    // link coalesces the ticks it misses.
    void startEngine();

    bool engineStarted = false;
};
} // namespace dhewm3
