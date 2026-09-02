/*
===========================================================================

dhewm 3 on eacp - Dear ImGui's platform half, and who gets an event.

The settings menu needs two backends under it: a platform one that feeds ImGuiIO
and a renderer one that draws ImDrawData. Both exist for eacp already, in
imgui-eacp - `Gui::DrawRenderer` and `Gui::KeyMap` - and neither of them is a
window system's event loop, which is what this file is.

**What is here is the part imgui-eacp cannot know**: that this ImGui is sharing a
keyboard and a mouse with a game. `Gui::ImGuiView` is a panel and owns its
events; the settings menu is an overlay over Doom 3, and every event has to be
decided rather than consumed. The rules for deciding are Doom 3's own, and they
are transcribed from the SDL host step 5 deleted rather than invented.

===========================================================================
*/

#pragma once

#include <eacp/Graphics/Graphics/Keyboard.h>
#include <eacp/Graphics/View/View.h>

namespace dhewm3
{
using namespace eacp;

namespace ImGuiBackend
{
#ifndef IMGUI_DISABLE

/*
    The producers, and each answers the question ProcessEvent(const SDL_Event*)
    used to: **has the menu used this, so that the game must not see it?**

    The rules are the SDL host's, and they are about Doom 3 rather than about
    SDL:

      - with no ImGui window open, nothing is ever consumed;
      - in keybind mode nothing is either, because the Bindings tab reads the
        pressed key out of the *engine's* state and the engine has to be given
        it;
      - a release is never consumed, so a button held before the menu took focus
        does not stay held for the rest of the run;
      - F1 to F12 are never consumed, so quickload and screenshot keep working
        over an open menu - which is what Doom 3's own menu does too.

    What each one does with the event before deciding is imgui-eacp's
    Gui::sendMouseButton and friends, so the port and Gui::ImGuiView feed an
    ImGuiIO through one implementation rather than two.
*/
bool keyEvent(const Graphics::KeyEvent& event, bool down);
bool mouseButton(const Graphics::MouseEvent& event, bool down);
bool mouseMotion(const Graphics::MouseEvent& event);
bool mouseWheel(const Graphics::MouseEvent& event);

// The pointer left the window, which nothing in the engine cares about and ImGui
// does: without it whatever was under the cursor stays hovered.
void mouseExited();

// Shift, Ctrl, Alt and Command. eacp reports them as state rather than as key
// events (plan.md §5, gap 9), exactly as the engine side of Input.cpp has to
// deal with, so they arrive here the same way: off each event that carries them,
// and off the once-a-frame poll.
void syncModifiers(const Graphics::ModifierKeys& modifiers);

// Whether the menu is taking the keyboard right now, and so whether a modifier
// going *down* is the game's. Asked separately because a modifier is state
// rather than an event on this host: there is no event for syncModifiers to
// consume and answer for, only a difference to decide who is told about.
bool capturesKeyboard();

// Key focus. Losing it releases everything ImGui thinks is held, for the reason
// the engine side does it: macOS delivers key-up to the key window alone.
void setFocus(bool hasFocus);

#else // IMGUI_DISABLE - there is no menu, so nothing is ever consumed

inline bool keyEvent(const Graphics::KeyEvent&, bool) { return false; }
inline bool mouseButton(const Graphics::MouseEvent&, bool) { return false; }
inline bool mouseMotion(const Graphics::MouseEvent&) { return false; }
inline bool mouseWheel(const Graphics::MouseEvent&) { return false; }
inline void mouseExited() {}
inline void syncModifiers(const Graphics::ModifierKeys&) {}
inline bool capturesKeyboard() { return false; }
inline void setFocus(bool) {}

#endif
} // namespace ImGuiBackend
} // namespace dhewm3
