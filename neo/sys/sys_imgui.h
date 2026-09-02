

#ifndef NEO_SYS_SYS_IMGUI_H_
#define NEO_SYS_SYS_IMGUI_H_

#ifndef IMGUI_DISABLE
#include "../libs/imgui/imgui.h"
#endif

namespace D3 {
namespace ImGuiHooks {

enum D3ImGuiWindow {
	D3_ImGuiWin_None        = 0,
	D3_ImGuiWin_Settings    = 1, // advanced dhewm3 settings menu
	D3_ImGuiWin_Demo        = 2, // ImGui demo window
	// next should be 4, then 8, etc so a bitmask can be used
};

#ifndef IMGUI_DISABLE

extern ImGuiContext* imguiCtx; // this is only here so IsImguiEnabled() can use it inline

inline bool IsImguiEnabled()
{
	return imguiCtx != NULL;
}

// Creates the ImGui context and everything that hangs off it - the style, the
// font, imgui.ini, the default F10 binding - and brings up the two backends
// under it. Called from the host once the window and the renderer exist
// (sys/eacp/GLimp.cpp), which is where the SDL host called it too.
//
// It took an SDL_Window and an SDL_GLContext as void* until step 7, to keep
// SDL's headers out of this one. The eacp host has nothing to pass: the platform
// backend reaches the window through the input layer that already tracks it, and
// the renderer backend is idRenderBackend, which is a global.
extern bool Init();

extern void Shutdown();

extern void OpenWindow( D3ImGuiWindow win );

extern void CloseWindow( D3ImGuiWindow win );

// enum D3ImGuiWindow values of all currently open imgui windows or-ed together
// (0 if none are open)
extern int GetOpenWindowsMask();

// for binding keys from an ImGui-based menu: send input events to dhewm3
// even if ImGui window has focus
extern void SetKeyBindMode( bool enable );

/*
	The platform backend's two questions to this file.

	They replace ProcessEvent(const void*), which took an SDL_Event and answered
	"has ImGui used this". The eacp host has no such type: its events arrive as
	four typed callbacks rather than as one tagged union, so deciding what ImGui
	consumed is done where they arrive (sys/eacp/ImGuiBackend.cpp) and what it
	needs from here is these two facts.
*/

// SetKeyBindMode( true ) is in force, which is the Bindings tab waiting for a
// key. Every event goes to the game then, whatever ImGui wants, because the game
// is what turns a key into the keyNum_t the binding is made of.
extern bool IsKeyBindMode();

// A key or button went down. The warning overlay disappears on one, which is
// the whole of what this is for.
extern void NotifyKeyDownEvent();

/*
	The two backends under the menu, which the host supplies - here that is
	sys/eacp/ImGuiBackend.cpp for the platform half and imgui-eacp's
	Gui::DrawRenderer, reached through idRenderBackend::DrawImGui, for the
	renderer half.

	Named rather than included, so this header stays free of any window system's
	and of any graphics API's: what a backend has to install on an ImGuiIO is the
	same shape on every host, and what it installs it *from* is not.

	The event producers are not here for exactly that reason. They take the
	host's own event types, so they belong in the host's own header; these four
	are what the menu asks of a backend whatever the backend is.
*/
namespace Backend {

// Install both halves onto the context Init() has just made: their names, the
// flags that say what the renderer can do, the clipboard, and whatever else
// ImGuiIO needs before its first frame.
extern bool Init();

// Unregister them. ImGui::DestroyContext asserts that a backend cleared what it
// registered, which is how it catches a context outliving its renderer.
extern void Shutdown();

// Once a frame, before ImGui::NewFrame(): the display's size and density, the
// time since the last one, and the pointer's shape.
extern void NewFrame();

// The window's backing scale, for imgui_scale's "auto".
extern float DisplayScale();

} //namespace Backend

// returns true if the system cursor should be shown because an ImGui menu is active
extern bool ShouldShowCursor();

// NewFrame() is called once per D3 frame, after all events have been gotten
// => ProcessEvent() has already been called (probably multiple times)
extern void NewFrame();

// called at the end of the D3 frame, when all other D3 rendering is done
// renders ImGui menus then
extern void EndFrame();

extern float GetScale();
extern void SetScale( float scale );

// show a red overlay-window at the center of the screen that contains
// a warning symbol (triangle with !) and the given text
// disappears after a few seconds or when a key is pressed or the mouse is moved
extern void ShowWarningOverlay( const char* text );

enum Style {
	Dhewm3,
	ImGui_Default,
	User
};

// set the overall style for ImGui: Both shape (sizes, roundings, etc) and colors
extern void SetImGuiStyle( Style style );

// set the default dhewm3 imgui style colors
extern void SetDhewm3StyleColors( ImGuiStyle* dst = nullptr );
extern void SetUserStyleColors();

// write current style settings (incl. colors) as userStyle
extern bool WriteUserStyle();

// copy current style to clipboard
extern void CopyCurrentStyle( bool onlyChanges );

#else // IMGUI_DISABLE - just stub out everything

inline bool IsImguiEnabled()
{
	return false;
}

inline bool Init()
{
	return false;
}

inline void Shutdown() {}

inline void SetKeyBindMode( bool enable ) {}

inline bool IsKeyBindMode() { return false; }

inline void NotifyKeyDownEvent() {}

inline bool ShouldShowCursor() { return false; }

inline void NewFrame() {}

inline void EndFrame() {}

inline void OpenWindow( D3ImGuiWindow win ) {}

inline void CloseWindow( D3ImGuiWindow win ) {}

inline int GetOpenWindowsMask() { return 0; }

inline float GetScale() { return 1.0f; }
inline void SetScale( float scale ) {}

inline void ShowWarningOverlay( const char* text ) {}

inline bool WriteUserStyle() { return false; }

#endif

}} //namespace D3::ImGuiHooks


#endif /* NEO_SYS_SYS_IMGUI_H_ */
