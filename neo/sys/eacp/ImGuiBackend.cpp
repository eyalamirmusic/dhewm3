/*
===========================================================================

dhewm 3 on eacp - Dear ImGui's platform half.

See ImGuiBackend.h for what this is and what it is not.

===========================================================================
*/

// eacp, imgui-eacp and Dear ImGui first, all for the same reason: idlib/Str.h
// turns strcmp and eight of its neighbours into macros, so any standard header
// pulled in after a Doom 3 one fails to compile - and these three are full of
// standard headers. Input.cpp says the same thing at more length.
#include "ImGuiBackend.h"

#include "Input.h"
#include "View.h"

#include <eacp/Graphics/Window/Window.h>

#ifndef IMGUI_DISABLE
  #include <imgui-eacp/ImGuiEacp.h>
#endif

#include <chrono>

#include "sys/platform.h"
#include "sys/sys_imgui.h"
#include "sys/sys_public.h"
#include "framework/Common.h"
#include "renderer/tr_local.h" // glConfig

#ifndef IMGUI_DISABLE

using eacp::Graphics::KeyEvent;
using eacp::Graphics::ModifierKeys;
using eacp::Graphics::MouseEvent;

namespace dhewm3
{
namespace ImGuiBackend
{
namespace
{
std::chrono::steady_clock::time_point lastFrameTime;
bool haveLastFrameTime = false;

ImGuiMouseCursor appliedCursor = ImGuiMouseCursor_Arrow;

// ImGui reads the pasted text through a pointer it does not own, so the copy has
// to outlive the call that returned it.
idStr clipboardText;

// Whether the menu could take an event at all. Both halves are the SDL host's
// rule: no context is a build where Init never ran, and no open window is a menu
// that is not on screen - in which case ImGui is not told about the event
// either, because it has nothing to do with it and the engine is about to be
// given it anyway.
bool menuIsOpen()
{
    return D3::ImGuiHooks::IsImguiEnabled() && D3::ImGuiHooks::GetOpenWindowsMask() != 0;
}

// F1 to F12 are the keys an open menu lets through, so that the engine's own
// shortcuts - quickload, quicksave, screenshot - still work with it on screen.
// Doom 3's menu does the same, and so did the SDL backend this replaces.
bool isFunctionKey(std::uint16_t keyCode)
{
    const auto key = Gui::toImGuiKey(keyCode);

    return key >= ImGuiKey_F1 && key <= ImGuiKey_F12;
}

const char* getClipboardText(ImGuiContext*)
{
    clipboardText.Clear();

    // Sys_GetClipboardData hands back a block the caller has to free, and
    // returns NULL for a pasteboard holding an image or nothing at all.
    if (char* data = Sys_GetClipboardData())
    {
        clipboardText = data;
        Sys_FreeClipboardData(data);
    }

    return clipboardText.c_str();
}

void setClipboardText(ImGuiContext*, const char* text)
{
    if (text != nullptr)
        Sys_SetClipboardData(text);
}

// The same event with its position measured from the picture rather than from
// the window.
//
// io.DisplaySize is glConfig.winWidth/winHeight, which since r_mode was
// honoured is the rectangle the frame is *fitted* into rather than the whole
// window - the window cannot be resized to the mode, so there may be black
// either side of the picture (contentRect, View.cpp). ImGui laid out in that
// rectangle has to be pointed at in it too, or every widget is a letterbox
// bar's width from where the cursor says it is.
MouseEvent inContentSpace(MouseEvent event)
{
    const auto content = dhewm3::contentRect();

    event.pos.x -= content.x;
    event.pos.y -= content.y;

    return event;
}
} // namespace

/*
================================================================================

    the producers

================================================================================
*/

bool keyEvent(const KeyEvent& event, bool down)
{
    if (!menuIsOpen())
        return false;

    // The modifiers, the key and - on a down that is not part of a shortcut -
    // the text, all in imgui-eacp's one implementation of those three rules.
    Gui::sendKey(ImGui::GetIO(), event, down);

    if (D3::ImGuiHooks::IsKeyBindMode())
    {
        // The Bindings tab is waiting for a key and reads the answer out of
        // idKeyInput, so the engine has to be given every event whatever ImGui
        // wants. This is also why the key still went to ImGui above: the popup
        // asking for it is an ImGui window and has to keep drawing.
        return false;
    }

    if (down)
        D3::ImGuiHooks::NotifyKeyDownEvent();

    if (!ImGui::GetIO().WantCaptureKeyboard)
        return false;

    // A release always reaches the engine, so that a key held before the menu
    // took focus is released rather than stuck down for the rest of the run.
    if (!down)
        return false;

    return !isFunctionKey(event.keyCode);
}

bool mouseButton(const MouseEvent& event, bool down)
{
    if (!menuIsOpen())
        return false;

    // eacp reports every button past the third as an undifferentiated `Other`
    // (plan.md §5, gap 15) and Gui::toImGuiButton reads that as the left one, so
    // the engine's own three are what this is asked about at all.
    if (event.button != Graphics::MouseButton::Left
        && event.button != Graphics::MouseButton::Right
        && event.button != Graphics::MouseButton::Middle)
        return false;

    Gui::sendMouseButton(ImGui::GetIO(), inContentSpace(event), down);

    if (D3::ImGuiHooks::IsKeyBindMode())
        return false;

    if (down)
        D3::ImGuiHooks::NotifyKeyDownEvent();

    // A release reaches the engine for the same reason a key's does.
    return ImGui::GetIO().WantCaptureMouse && down;
}

bool mouseMotion(const MouseEvent& event)
{
    if (!menuIsOpen())
        return false;

    // In logical points with the origin at the top left, which is the space
    // io.DisplaySize is in - see Backend::NewFrame. eacp's backing views set
    // isFlipped, so this is already y-down and nothing has to be turned over.
    Gui::sendMousePosition(ImGui::GetIO(), inContentSpace(event));

    if (D3::ImGuiHooks::IsKeyBindMode())
        return false;

    return ImGui::GetIO().WantCaptureMouse;
}

bool mouseWheel(const MouseEvent& event)
{
    if (!menuIsOpen())
        return false;

    // FontSizeBase is what says how far one wheel notch scrolls, which is what
    // a trackpad's points have to be divided by; imgui-eacp owns that
    // conversion, and the engine's own K_MWHEELUP threshold is a separate
    // question answered in Input.cpp.
    Gui::sendMouseWheel(ImGui::GetIO(), inContentSpace(event), ImGui::GetStyle().FontSizeBase);

    if (D3::ImGuiHooks::IsKeyBindMode())
        return false;

    D3::ImGuiHooks::NotifyKeyDownEvent();

    return ImGui::GetIO().WantCaptureMouse;
}

void mouseExited()
{
    if (!D3::ImGuiHooks::IsImguiEnabled())
        return;

    Gui::sendMouseExited(ImGui::GetIO());
}

void syncModifiers(const ModifierKeys& modifiers)
{
    if (!D3::ImGuiHooks::IsImguiEnabled())
        return;

    // The four ImGui names and no Left/Right pair beside them, because
    // ModifierKeys does not carry the side. Nothing in the menu asks: a shortcut
    // is ImGuiMod_Ctrl, and the Bindings tab reads the engine's state rather
    // than ImGui's.
    Gui::addModifiers(ImGui::GetIO(), modifiers);
}

bool capturesKeyboard()
{
    if (!menuIsOpen() || D3::ImGuiHooks::IsKeyBindMode())
        return false;

    return ImGui::GetIO().WantCaptureKeyboard;
}

void setFocus(bool hasFocus)
{
    if (!D3::ImGuiHooks::IsImguiEnabled())
        return;

    // Everything held is released by AddFocusEvent(false) itself, which is what
    // ImGui's own backends rely on. macOS delivers key-up to the key window
    // alone, so without it a Cmd-Tab with a modifier down would leave every
    // click afterwards looking like a Cmd-click.
    ImGui::GetIO().AddFocusEvent(hasFocus);
}
} // namespace ImGuiBackend
} // namespace dhewm3

/*
================================================================================

    what sys/sys_imgui.cpp calls

    Declared in sys/sys_imgui.h rather than in ImGuiBackend.h, because these four
    say nothing about eacp: every host has a display size, a time step, a
    clipboard and a pointer.

================================================================================
*/

namespace D3
{
namespace ImGuiHooks
{
namespace Backend
{
bool Init()
{
    auto& io = ImGui::GetIO();

    io.BackendPlatformName = "eacp";

    // The renderer half's own claims, made here rather than in sys_imgui.cpp
    // because this is the file that knows which renderer is linked. Both flags
    // are things Gui::DrawRenderer does and the seam's DrawImGui documents:
    // HasTextures lets ImGui grow the atlas a glyph at a time through
    // ImDrawData::Textures, and HasVtxOffset lets it keep a window's geometry in
    // one draw list past 64k vertices, because every draw carries a base vertex.
    io.BackendRendererName = "imgui-eacp";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;

    auto& platformIO = ImGui::GetPlatformIO();

    platformIO.Platform_GetClipboardTextFn = dhewm3::ImGuiBackend::getClipboardText;
    platformIO.Platform_SetClipboardTextFn = dhewm3::ImGuiBackend::setClipboardText;

    dhewm3::ImGuiBackend::haveLastFrameTime = false;

    return true;
}

void Shutdown()
{
    auto& io = ImGui::GetIO();

    // ImGui::Shutdown asserts that a backend cleared what it registered, which
    // is how it catches a context outliving the thing that was drawing it.
    io.BackendPlatformName = nullptr;
    io.BackendRendererName = nullptr;
    io.BackendPlatformUserData = nullptr;
    io.BackendRendererUserData = nullptr;

    dhewm3::ImGuiBackend::clipboardText.Clear();
    dhewm3::ImGuiBackend::haveLastFrameTime = false;
}

void NewFrame()
{
    auto& io = ImGui::GetIO();

    /*
        The display, in the two sizes GLimp_Init measured and for the reason it
        measured both. Logical points, like the rest of eacp: DisplaySize is what
        a mouse coordinate arrives in and therefore what ImGui has to lay itself
        out in for the pointer to land on what it is over, and the framebuffer
        scale is what turns a clip rectangle into the render target's pixels -
        which is the only thing imgui-eacp measures in pixels.

        A ratio rather than the window's backing scale even though the two are
        the same number, and the difference is the point: the product is then
        exactly vidWidth, which is what EnsureFrameTarget allocated the target
        at, so no rounding can put a scissor a pixel outside it.
    */
    if (glConfig.winWidth > 0 && glConfig.winHeight > 0)
    {
        io.DisplaySize = ImVec2((float) glConfig.winWidth, (float) glConfig.winHeight);
        io.DisplayFramebufferScale =
            ImVec2((float) glConfig.vidWidth / (float) glConfig.winWidth,
                   (float) glConfig.vidHeight / (float) glConfig.winHeight);
    }

    // A real clock rather than the engine's, which stops: com_fixedTic pins the
    // game's tick to the frame, and opening the settings menu sets g_stoptime,
    // which freezes it outright. A menu whose caret blink and scroll inertia are
    // driven by frozen time does not animate at all.
    const auto now = std::chrono::steady_clock::now();

    if (dhewm3::ImGuiBackend::haveLastFrameTime)
    {
        const auto elapsed =
            std::chrono::duration<float>(now - dhewm3::ImGuiBackend::lastFrameTime)
                .count();

        // ImGui rejects a non-positive delta, and a stall long enough to matter
        // should step the animations rather than jump them.
        io.DeltaTime = std::clamp(elapsed, 1.0f / 1000.0f, 0.1f);
    }
    else
    {
        io.DeltaTime = 1.0f / 60.0f;
        dhewm3::ImGuiBackend::haveLastFrameTime = true;
    }

    dhewm3::ImGuiBackend::lastFrameTime = now;

    // The pointer's shape, which is ImGui's to choose while the mouse is over one
    // of its windows. NoMouseCursorChange is set by NewFrame() in sys_imgui.cpp
    // on exactly the frames where it is not - Doom 3 is drawing its own cursor
    // then, and this must not fight it.
    if ((io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) == 0)
    {
        const auto wanted = ImGui::GetMouseCursor();

        if (wanted != dhewm3::ImGuiBackend::appliedCursor)
        {
            dhewm3::ImGuiBackend::appliedCursor = wanted;

            if (auto* view = dhewm3::Input::getView())
                view->setMouseCursor(eacp::Gui::toMouseCursor(wanted));
        }
    }
    else
    {
        dhewm3::ImGuiBackend::appliedCursor = ImGuiMouseCursor_Arrow;
    }
}

/*
    What sys_imgui.cpp multiplies the menu's font sizes by when imgui_scale is
    -1, and on this host the answer is always one.

    It is asking how many pixels a *point* of menu is worth, because the SDL
    backend it was written for hands ImGui a DisplaySize in pixels: on a 150%
    Windows display a 13-unit font is 13 pixels there, which is unreadably small,
    so the scale has to put the density back.

    eacp lays views out in points and Backend::NewFrame passes those points
    through as DisplaySize, with the density carried separately in
    DisplayFramebufferScale - so a 13-unit font is already 13 points tall on any
    display and there is nothing left to correct. Answering the backing scale
    here would double the menu on a 2x panel.

    The reason this is worth saying out loud rather than leaving to
    GetDefaultScale's own early-out is that the early-out reads
    `winWidth != vidWidth`, which was a proxy for "the framebuffer is scaled for
    me" and stopped being true the day r_mode started deciding vidWidth: on this
    host the two are equal whenever the mode happens to match the window.
*/
float DisplayScale()
{
    return 1.0f;
}
} // namespace Backend
} // namespace ImGuiHooks
} // namespace D3

#endif // !IMGUI_DISABLE
