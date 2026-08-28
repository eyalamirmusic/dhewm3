/*
===========================================================================

dhewm 3 on eacp - the application object Apps::run<T>() owns.

===========================================================================
*/

#pragma once

#include "Input.h"
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

        // Key focus, which the input layer needs for two things: the grab is
        // released while another app is in front, and everything held is
        // released with it. macOS delivers key-up to the key window alone, so a
        // Cmd-Tab with W down would otherwise leave the player walking into a
        // wall until that key is pressed and released again.
        //
        // On the window rather than on the view because that is where the
        // platform reports it: a view has focus within a window, and this is
        // the window having it within the session.
        window.events.onActivationChanged = [](bool isKey)
        { Input::setFocus(isKey); };
    }

    Graphics::Window window {windowOptions()};
    View view;
};
} // namespace dhewm3
