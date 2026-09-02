/*
===========================================================================

dhewm 3 on eacp - what the window hands the event queue.

The queue and every Sys_* entry point live in Input.cpp; this is the half the
view calls. It is its own header rather than more methods on View because the
two are different jobs: View is the engine's host - it starts it, steps it and
will draw it - and this is the platform's input, which happens to arrive as
callbacks on a view.

Everything here runs on the main thread. eacp delivers input from the run loop
and GPUView::update drives common->Frame() off the display link, both on that
thread, so the queue behind this needs no lock: it is filled between frames and
drained inside one.

===========================================================================
*/

#pragma once

#include <eacp/Graphics/Graphics/Keyboard.h>
#include <eacp/Graphics/View/View.h>

namespace dhewm3
{
using namespace eacp;

struct View;

namespace Input
{
// The view whose window owns the pointer. The grab - GLimp_GrabInput, called
// once a frame out of Sys_GenerateEvents - has to reach a Window to lock the
// mouse, and the view is what has one.
//
// Set by View's constructor and cleared by its destructor rather than handed
// over at startup, because the engine's shutdown runs *through* that
// destructor: the grab has to stop finding a window at the moment the view
// stops being one, not at the moment the process exits.
void setView(View* view);
View* getView();

// The producers. Each is a no-op until common->Init has run: the window exists
// from before Apps::run's loop starts and the engine is not started until the
// first refresh, so a key pressed in that gap would be resolved against cvars
// that are not registered yet, for an engine that could not receive it.
void keyEvent(const Graphics::KeyEvent& event, bool down);
void mouseButton(const Graphics::MouseEvent& event, bool down);
void mouseMotion(const Graphics::MouseEvent& event);
void mouseWheel(const Graphics::MouseEvent& event);

// The pointer left the window, which is the settings menu's business and not
// the engine's - Doom 3 has no notion of a cursor that is nowhere.
void mouseExited();

// eacp reports Shift, Ctrl, Alt and Command as modifier *state* and never as
// key events (plan.md §5, gap 9), and Doom 3 binds all but Command as ordinary
// keys - _attack, _strafe and _speed in the stock config. So they are polled
// once a frame and the difference is turned into the down/up pair the engine
// would otherwise never see.
void syncModifiers(const Graphics::ModifierKeys& current);

// Key focus. Losing it releases everything held: macOS delivers key-up to the
// key window alone, so a Cmd-Tab with W down would otherwise leave the player
// walking into a wall until the key is pressed and released again.
void setFocus(bool hasFocus);
} // namespace Input
} // namespace dhewm3
