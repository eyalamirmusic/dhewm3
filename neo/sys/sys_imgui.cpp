
/*
The in-game settings menu, with no backend under it.

Dear ImGui needs two: a platform backend that feeds it events and a renderer
backend that draws its vertex buffers. This tree had ImGui_ImplSDL3 and
ImGui_ImplOpenGL2, and step 5 deleted both of them with the host they belonged
to. eacp has neither yet - the port's renderer is idRenderBackendEacp rather
than raw Metal, so ImGui's own imgui_impl_metal.mm is not simply a drop-in, and
the state save/restore that used to sit in EndFrame() has no analogue here.

So the menu is compiled and dark rather than deleted: it is a headline dhewm3
feature and porting it is a step of its own (plan.md). Init() is what would
create the ImGui context, and nothing calls it on this build - it returns false
and says why. Everything else here holds imguiCtx == NULL as the thing that
means "no menu", and refuses on it rather than dereferencing it.
*/

#define IMGUI_DEFINE_MATH_OPERATORS

#include "sys_imgui.h"

#include "framework/Common.h"
#include "framework/KeyInput.h"
#include "framework/Session_local.h" // sessLocal.GetActiveMenu()
#include "renderer/tr_local.h" // glconfig
#include "ui/DeviceContext.h"
#include "ui/UserInterface.h"

extern void Com_DrawDhewm3SettingsMenu(); // in framework/dhewm3SettingsMenu.cpp
extern void Com_OpenCloseDhewm3SettingsMenu( bool open ); // ditto

static idCVar imgui_scale( "imgui_scale", "-1.0", CVAR_SYSTEM|CVAR_FLOAT|CVAR_ARCHIVE, "factor to scale ImGUI menus by (-1: auto)" ); // TODO: limit values?

idCVar imgui_style( "imgui_style", "0", CVAR_SYSTEM|CVAR_INTEGER|CVAR_ARCHIVE, "Which ImGui style to use. 0: Dhewm3 theme, 1: Default ImGui theme, 2: User theme", 0.0f, 2.0f );

extern idCVar r_scaleMenusTo43;

// implemented in imgui_savestyle.cpp
namespace DG {
// writes the given ImGuiStyle to the given filename (opened with fopen())
// returns true on success, false if opening the file failed
extern bool WriteImGuiStyle( const ImGuiStyle& style, const char* filename );

// reads the the given filename (opened with fopen())
// and sets the given ImGuiStyle accordingly.
// if any attributes/colors/behaviors are missing the the file,
// they are not modified in style, so it probably makes sense to initialize
// style to a sane default before calling this function.
// returns true on success, false if opening the file failed
extern bool ReadImGuiStyle( ImGuiStyle& style, const char* filename );

// generate C++ code that replicates the given style into a text buffer
// (that you can write to a file or set the clipboard from or whatever)
// if refStyle is set, only differences in style compared to refStyle are written
extern ImGuiTextBuffer WriteImGuiStyleToCode( const ImGuiStyle& style, const ImGuiStyle* refStyle = nullptr );
} //namespace DG

namespace D3 {
namespace ImGuiHooks {

#ifdef _MSC_VER
  // Visual C++ (at least up to some 2019 version) doesn't support string literals
  // with more than 65535 bytes, so the base85-encoded version won't work here..
  // this alternative doesn't work with Big Endian, but that's not overly relevant for Windows.
  #include "proggyvector_font.h"
#else // proper compilers that support longer string literals
  #include "proggyvector_font_base85.h"
#endif

ImGuiContext* imguiCtx = NULL;
static bool haveNewFrame = false;
static int openImguiWindows = 0; // or-ed enum D3ImGuiWindow values

static ImGuiStyle userStyle;

// was there a key down or button (mouse/gamepad) down event this frame?
// used to make the warning overlay disappear
static bool hadKeyDownEvent = false;

static idStr warningOverlayText;
static double warningOverlayStartTime = -100.0;
static ImVec2 warningOverlayStartPos;

idStr GetUserStyleFilename()
{
	idStr ret( cvarSystem->GetCVarString( "fs_configpath" ) );
	ret += "/user.imstyle";
	return ret;
}

static void UpdateWarningOverlay()
{
	double timeNow = ImGui::GetTime();
	if ( timeNow - warningOverlayStartTime > 4.0f ) {
		warningOverlayStartTime = -100.0f;
		return;
	}

	// also hide if a key was pressed or maybe even if the mouse was moved (too much)
	ImVec2 mdv = ImGui::GetMousePos() - warningOverlayStartPos; // Mouse Delta Vector
	float mouseDelta = sqrtf( mdv.x * mdv.x + mdv.y * mdv.y );
	const float fontSize = ImGui::GetFontSize();
	if ( mouseDelta > fontSize * 4.0f || hadKeyDownEvent ) {
		warningOverlayStartTime = -100.0f;
		return;
	}

	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::PushStyleColor( ImGuiCol_WindowBg, ImVec4(1.0f, 0.4f, 0.4f, 0.6f) );
	float padSize = fontSize * 2.0f;
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2(padSize, padSize) );

	int winFlags = ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove
			| ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
	ImGui::Begin("WarningOverlay", NULL, winFlags);

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 points[] = {
		{0, 40}, {40, 40}, {20, 0}, // triangle
		{20, 12}, {20, 28}, // line
		{20, 33} // dot
	};

	float iconScale = 1.0f; // TODO: global scale also used for fontsize

	ImVec2 offset = ImGui::GetWindowPos() + ImVec2(fontSize, fontSize);
	for ( ImVec2& v : points ) {
		v.x = roundf( v.x * iconScale );
		v.y = roundf( v.y * iconScale );
		v += offset;
	}

	ImU32 color = ImGui::GetColorU32( ImVec4(0.1f, 0.1f, 0.1f, 1.0f) );

	drawList->AddTriangle( points[0], points[1], points[2], color, roundf( iconScale * 4.0f ) );

	drawList->AddPolyline( points+3, 2, color, 0, roundf( iconScale * 3.0f ) );

	float dotRadius = 2.0f * iconScale;
	drawList->AddEllipseFilled( points[5], ImVec2(dotRadius, dotRadius), color, 0, 6 );

	ImGui::Indent( 40.0f * iconScale );
	ImGui::TextUnformatted( warningOverlayText.c_str() );

	ImGui::End();

	ImGui::PopStyleVar(); // WindowPadding
	ImGui::PopStyleColor(); // WindowBg
}

void ShowWarningOverlay( const char* text )
{
	warningOverlayText = text;
	warningOverlayStartTime = ImGui::GetTime();
	warningOverlayStartPos = ImGui::GetMousePos();
}

static float GetDefaultScale()
{
	// This used to ask the platform backend for the window's display scale.
	// There is no platform backend on this build, and the one thing the engine
	// already knows is enough for the HighDPI case: in HighDPI mode the font
	// sizes are already scaled to window coordinates.
	return 1.0f;
}

float GetScale()
{
	float ret = imgui_scale.GetFloat();
	if (ret < 0.0f) {
		ret = GetDefaultScale();
	}
	return ret;
}

void SetScale( float scale )
{
	imgui_scale.SetFloat( scale );
}

static bool imgui_initialized = false;

// The two void* were an SDL_Window and an SDL_GLContext, to keep SDL's headers
// out of sys_imgui.h. Nothing calls this on the eacp host: the ImGui context is
// created here and only here, so leaving it uncreated is what keeps every other
// entry point below dark. Creating one without a platform or renderer backend
// would be worse than not creating it - the menu would take input it cannot
// draw a response to.
bool Init(void* window, void* renderContext)
{
	common->Warning( "The dhewm3 settings menu is not available on this build yet: "
	                 "Dear ImGui has no backend for the eacp host.\n" );
	return false;
}

void Shutdown()
{
	if ( imgui_initialized ) {
		common->Printf( "Shutting down ImGui\n" );

		ImGui::DestroyContext( imguiCtx );
		imguiCtx = NULL;
		imgui_initialized = false;
	}
}

// NewFrame() is called once per D3 frame, after all events have been gotten
// => ProcessEvent() has already been called (probably multiple times)
void NewFrame()
{
	D3P_ScopedCPUSample(Imgui_NewFrame);

	// There is no context at all until Init() makes one, and on this build
	// nothing does. Everything below would then be called on a NULL ImGui
	// context - which crashed rather than did nothing, because the "all windows
	// closed" early-out two blocks down still runs a couple of frames first.
	if ( imguiCtx == NULL ) {
		return;
	}

	// it can happen that NewFrame() is called without EndFrame() having been called
	// after the last NewFrame() call, for example when D3Radiant is active and in
	// idSessionLocal::UpdateScreen() Sys_IsWindowVisible() returns false.
	// In that case, end the previous frame here so it's ended at all.
	if ( haveNewFrame ) {
		EndFrame();
	}

	// even if all windows are closed, still run a few frames
	// so ImGui also recognizes internally that all windows are closed
	// and e.g. ImGuiCond_Appearing works as intended
	static int framesAfterAllWindowsClosed = 0;
	if ( openImguiWindows == 0 ) {
		if ( framesAfterAllWindowsClosed > 1 )
			return;
		else
			++framesAfterAllWindowsClosed;
	} else {
		framesAfterAllWindowsClosed = 0;
	}

	if ( imgui_scale.IsModified() ) {
		imgui_scale.ClearModified();
		float scale = imgui_scale.GetFloat();
		if (scale < 0.0f) {
			scale = GetDefaultScale();
		}

  	ImGuiStyle& style = ImGui::GetStyle();
		style.FontScaleDpi = scale;
	}

	// Start the Dear ImGui frame
	if ( ShouldShowCursor() )
		ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
	else
		ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

	ImGui::NewFrame();
	haveNewFrame = true;

	UpdateWarningOverlay();

	if (openImguiWindows & D3_ImGuiWin_Settings) {
		Com_DrawDhewm3SettingsMenu();
	}

	if (openImguiWindows & D3_ImGuiWin_Demo) {
		bool show_demo_window = true;
		ImGui::ShowDemoWindow(&show_demo_window);
		if(!show_demo_window)
			CloseWindow(D3_ImGuiWin_Demo);
	}
}

bool keybindModeEnabled = false;

// Was called with every SDL event by Sys_GetEvent(), and returned true if ImGui
// had handled it. Deciding that is the platform backend's job - it is the half
// that reads a window system's events and writes ImGuiIO - and there is none
// here, so no event is ever ImGui's and the engine gets all of them. The eacp
// input layer does not call this at all; the declaration stays because a host
// that grows a backend will want it back.
bool ProcessEvent(const void* event)
{
	return false;
}

void SetKeyBindMode( bool enable )
{
	keybindModeEnabled = enable;
	// make sure no keys are registered as down, neither when entering nor when exiting keybind mode
	idKeyInput::ClearStates();
}

bool ShouldShowCursor()
{
	if ( sessLocal.GetActiveMenu() == nullptr ) {
		// when ingame, render the ImGui/SDL/system cursor if an ImGui window is open
		// because dhewm3 does *not* render its own cursor outside ImGui windows.
		// additionally, only show it if an ImGui window has focus - this allows you
		// to click outside the ImGui window to give Doom3 focus and look around.
		// You can get focus on the ImGui window again by clicking while the invisible
		//  cursor is over the window (things in it still get highlighted), or by
		// opening the main (Esc) or by opening the Dhewm3 Settings window (F10, usually),
		// which will either open it focused or give an ImGui window focus if it
		// was open but unfocused.
		// TODO: Might be nice to have a keyboard shortcut to give focus to any open
		//       ImGui window, maybe Pause?
		return openImguiWindows != 0 && ImGui::IsWindowFocused( ImGuiFocusedFlags_AnyWindow );
	} else {
		// if we're in a menu (probably main menu), dhewm3 renders a cursor for it,
		// so only show the ImGui cursor when the mouse cursor is over an ImGui window
		// or in one of the black bars where Doom3's cursor isn't rendered in
		// non 4:3 resolutions
		if ( openImguiWindows == 0 ) {
			return false; // no open ImGui window => no ImGui cursor
		}
		if ( ImGui::GetIO().WantCaptureMouse ) {
			return true; // over an ImGui window => definitely want ImGui cursor
		}
		// if scaling Doom3 menus to 4:3 is enabled and the cursor is currently
		// in a black bar (Doom3 cursor is not drawn there), show the ImGui cursor
		if ( idUserInterface::IsUserInterfaceScaledTo43( sessLocal.GetActiveMenu() ) ) {
			ImVec2 mousePos = ImGui::GetMousePos();
			float w = glConfig.winWidth;
			float h = glConfig.winHeight;
			float aspectRatio = w/h;
			static const float virtualAspectRatio = float(VIRTUAL_WIDTH)/float(VIRTUAL_HEIGHT); // 4:3 = 1.333
			if(aspectRatio > 1.4f) {
				// widescreen (4:3 is 1.333 3:2 is 1.5, 16:10 is 1.6, 16:9 is 1.7778)
				// => we need to scale and offset w to get the width of the black bars
				float scaleX = virtualAspectRatio/aspectRatio;
				float offsetX = (1.0f - scaleX) * w * 0.5f; // (w - scale*w)/2
				if ( mousePos.x <= offsetX || mousePos.x >= w - offsetX ) {
					return true;
				}
			} else if(aspectRatio < 1.24f) {
				// portrait-mode, "thinner" than 5:4 (which is 1.25)
				// => we need to scale and offset h to get the height of the black bars
				// it's analogue to the other case, but inverted and with height and Y
				float scaleY = aspectRatio/virtualAspectRatio;
				float offsetY = (1.0f - scaleY)* h * 0.5f; // (h - scale*h)/2
				if ( mousePos.y <= offsetY || mousePos.y >= h - offsetY ) {
					return true;
				}
			}
		}
		return false;
	}
}

void EndFrame()
{
	// Called every frame from RB_SwapBuffers, so this is the guard that matters
	// most. NewFrame() has always had one; this one had none, which made an open
	// window on a build with no context a null dereference two calls later.
	if ( imguiCtx == NULL ) {
		return;
	}

	if (openImguiWindows == 0 && !haveNewFrame)
		return;

	// I think this can happen if we're not coming from idCommon::Frame() but screenshot or sth
	if ( !haveNewFrame ) {
		NewFrame();
	}
	haveNewFrame = false;
	ImGui::Render();

	// This is where the renderer backend drew ImGui's draw data, wrapped in
	// save/restore of the fixed-function GL state Doom 3 had left behind. Both
	// halves went with the GL backend; the draw data is built and dropped.

	// reset this at the end of each frame, will be set again by ProcessEvent()
	if ( hadKeyDownEvent ) {
		hadKeyDownEvent = false;
	}
}


void OpenWindow( D3ImGuiWindow win )
{
	if ( imguiCtx == NULL ) {
		// no context, so no window to open and nothing that could draw one.
		// SetNextWindowFocus() below dereferences the context unconditionally,
		// which is what made `dhewm3Settings` a crash rather than a no-op.
		common->Printf( "The dhewm3 settings menu is not available on this build yet: "
		                "Dear ImGui has no backend for the eacp host.\n" );
		return;
	}

	if ( openImguiWindows & win )
		return; // already open

	ImGui::SetNextWindowFocus();

	switch ( win ) {
		case D3_ImGuiWin_Settings:
			Com_OpenCloseDhewm3SettingsMenu( true );
			break;
		// TODO: other windows that need explicit opening
	}

	openImguiWindows |= win;
}

void CloseWindow( D3ImGuiWindow win )
{
	if ( imguiCtx == NULL ) {
		return; // nothing was ever opened
	}

	if ( (openImguiWindows & win) == 0 )
		return; // already closed

	switch ( win ) {
		case D3_ImGuiWin_Settings:
			Com_OpenCloseDhewm3SettingsMenu( false );
			break;
		// TODO: other windows that need explicit closing
	}

	openImguiWindows &= ~win;
}

int GetOpenWindowsMask()
{
	return openImguiWindows;
}

ImGuiStyle GetImGuiStyle( Style d3style )
{
	ImGuiStyle style; // default style
	if ( d3style == Style::Dhewm3 ) {
		// make it look a bit nicer with rounded edges
		style.WindowRounding = 2.0f;
		style.FrameRounding = 3.0f;
		style.FramePadding = ImVec2( 6.0f, 3.0f );
		//style.ChildRounding = 6.0f;
		style.ScrollbarRounding = 8.0f;
		style.GrabRounding = 3.0f;
		style.PopupRounding = 2.0f;
		SetDhewm3StyleColors( &style );
	} else if ( d3style == Style::User ) {
		style = userStyle;
	} else {
		assert( d3style == Style::ImGui_Default && "invalid/unknown style" );
		ImGui::StyleColorsDark( &style );
	}
	return style;
}

void SetImGuiStyle( Style d3style )
{
	ImGui::GetStyle() = GetImGuiStyle( d3style );
}

void SetDhewm3StyleColors( ImGuiStyle* dst )
{
	if ( dst == nullptr )
		dst = &ImGui::GetStyle();
	ImGui::StyleColorsDark( dst );
	ImVec4* colors = dst->Colors;
	//colors[ImGuiCol_TitleBg]       = ImVec4(0.28f, 0.36f, 0.48f, 0.88f);
	//colors[ImGuiCol_TitleBg]       = ImVec4(0.09f, 0.23f, 0.22f, 0.90f);
	//colors[ImGuiCol_TitleBgActive] = ImVec4(0.02f, 0.52f, 0.53f, 1.00f);
	colors[ImGuiCol_TitleBg]       = ImVec4(0.09f, 0.13f, 0.12f, 0.90f);
	colors[ImGuiCol_TitleBgActive] = ImVec4(0.03f, 0.33f, 0.33f, 1.00f);
	//colors[ImGuiCol_TitleBg]       = ImVec4(0.12f, 0.17f, 0.16f, 0.90f);
	colors[ImGuiCol_TabHovered]    = ImVec4(0.42f, 0.69f, 1.00f, 0.80f);
	colors[ImGuiCol_TabSelected]     = ImVec4(0.24f, 0.51f, 0.83f, 1.00f);
}

void SetUserStyleColors()
{
	ImGuiStyle& style = ImGui::GetStyle();
	for ( int i=0; i < ImGuiCol_COUNT; ++i ) {
		style.Colors[i] = userStyle.Colors[i];
	}
}

bool WriteUserStyle()
{
	userStyle = ImGui::GetStyle();
	if ( !DG::WriteImGuiStyle( ImGui::GetStyle(), GetUserStyleFilename() ) ) {
		common->Warning( "Couldn't write ImGui userstyle!\n" );
		return false;
	}
	return true;
}

void CopyCurrentStyle( bool onlyChanges )
{
	ImGuiStyle refStyle = GetImGuiStyle( Style::ImGui_Default );
	ImGuiTextBuffer buf = DG::WriteImGuiStyleToCode( ImGui::GetStyle(), onlyChanges ? &refStyle : nullptr );
	Sys_SetClipboardData( buf.c_str() );
}

}} //namespace D3::ImGuiHooks
