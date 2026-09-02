
/*
The in-game settings menu, and the half of it that belongs to neither backend.

Dear ImGui needs two under it: a platform backend that feeds ImGuiIO and a
renderer backend that draws ImDrawData. This tree had ImGui_ImplSDL3 and
ImGui_ImplOpenGL2, and step 5 deleted both with the host they belonged to; step
7 gave it the two the eacp host needs, and neither of them is here. They are
imgui-eacp's - Dear ImGui's own eacp integration - reached through
D3::ImGuiHooks::Backend for the platform half and idRenderBackend::DrawImGui for
the renderer one, because triangles with a texture, a scissor and a blend are a
thing only a backend can say.

**So this file names no window system and no graphics API**, which it did not
quite manage before: it included SDL for the display scale and qgl for the state
save around ImGui_ImplOpenGL2_RenderDrawData. What is left is the menu itself -
the context, the style, the fonts, which windows are open, and the rules for who
gets an event - and it is the same file on any host that grows the two backends.
*/

#define IMGUI_DEFINE_MATH_OPERATORS

#include "sys_imgui.h"

#include "framework/Common.h"
#include "framework/KeyInput.h"
#include "framework/Session_local.h" // sessLocal.GetActiveMenu()
#include "renderer/RenderBackend.h" // renderBackend->DrawImGui()
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
	if ( glConfig.winWidth != glConfig.vidWidth ) {
		// In HighDPI mode the font sizes are already scaled to window
		// coordinates: ImGuiIO::DisplaySize is in points and the rasterizer
		// density follows the framebuffer scale, so the glyphs come out sharp
		// at the size they were asked for and nothing here has to double them.
		return 1.0f;
	}

	float ret = Backend::DisplayScale();

	// Validate that the reported scale is a reasonable size
	// For example: if xrandr fails to read the EDID of the display,
	// a default value 1mm x 1mm will be reported, resulting in an
	// absurdly high DPI
	if ( ret <= 0.0f || ret > 10.0f ) {
		return 1.0f;
	}

	ret = round(ret*2.0)*0.5; // round to .0 or .5
	return ret;
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

// The ImGui context is created here and only here, so imguiCtx == NULL stays
// what "no menu" means everywhere below: a host with no platform backend simply
// never reaches this, and every entry point holds the same guard rather than
// dereferencing a context nothing could draw a response through.
bool Init()
{
	common->Printf( "Initializing ImGui\n" );

	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	imguiCtx = ImGui::CreateContext();
	if ( imguiCtx == NULL ) {
		common->Warning( "Failed to create ImGui Context!\n" );
		return false;
	}
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

	static idStr iniPath;
	iniPath = cvarSystem->GetCVarString( "fs_configpath" );
	iniPath += "/imgui.ini";
	io.IniFilename = iniPath.c_str();

  // Setup styles
	SetImGuiStyle( Style::Dhewm3 );
	userStyle = ImGui::GetStyle(); // set dhewm3 style as default, in case the user style is missing values
	if ( DG::ReadImGuiStyle( userStyle, GetUserStyleFilename() ) && imgui_style.GetInteger() == 2 ) {
		ImGui::GetStyle() = userStyle;
	} else if ( imgui_style.GetInteger() == 1 ) {
		ImGui::GetStyle() = ImGuiStyle();
		ImGui::StyleColorsDark();
	}

	imgui_scale.SetModified();

	// Setup fonts, size will come from style.FontSizeBase
#ifdef _MSC_VER
	io.Fonts->AddFontFromMemoryCompressedTTF(ProggyVector_compressed_data, ProggyVector_compressed_size);
#else
	io.Fonts->AddFontFromMemoryCompressedBase85TTF(ProggyVector_compressed_data_base85);
#endif

	// The backends last, so that everything they may want to look at - the
	// style, the font, the config flags - is already there. This is where
	// ImGui is told what the renderer can do, which is a fact about whichever
	// backend the host linked rather than about this file.
	if ( !Backend::Init() ) {
		ImGui::DestroyContext( imguiCtx );
		imguiCtx = NULL;
		common->Warning( "Failed to initialize the ImGui backends!\n" );
		return false;
	}

	const char* f10bind = idKeyInput::GetBinding( K_F10 );
	if ( f10bind && f10bind[0] != '\0' ) {
		if ( idStr::Icmp( f10bind, "dhewm3Settings" ) != 0 ) {
			// if F10 is already bound, but not to dhewm3Settings, show a message
			common->Printf( "... the F10 key is already bound to '%s', otherwise it could be used to open the dhewm3 Settings Menu\n" , f10bind );
		}
	} else {
		idKeyInput::SetBinding( K_F10, "dhewm3Settings" );
	}

	imgui_initialized = true;
	return true;
}

void Shutdown()
{
	if ( imgui_initialized ) {
		common->Printf( "Shutting down ImGui\n" );

		Backend::Shutdown();

		// The renderer half has already let its textures go: the backend's own
		// Shutdown runs before GLimp_Shutdown, which is what calls this, so the
		// device the textures belong to outlives them by exactly the right
		// margin (idRenderSystemLocal::ShutdownOpenGL).
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

	// Init() is called from the host once the renderer is up, so there is no
	// context at all when the renderer never started: com_skipRenderer 1, a
	// dedicated server, or an Init that failed. Everything below would then be
	// called on a NULL ImGui context - which crashed rather than did nothing,
	// because the "all windows closed" early-out two blocks down still runs a
	// couple of frames first.
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

	// Before ImGui::NewFrame, because it is what the frame is measured against:
	// the display size, its density and the time step.
	Backend::NewFrame();

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

void SetKeyBindMode( bool enable )
{
	keybindModeEnabled = enable;
	// make sure no keys are registered as down, neither when entering nor when exiting keybind mode
	idKeyInput::ClearStates();
}

bool IsKeyBindMode()
{
	return keybindModeEnabled;
}

void NotifyKeyDownEvent()
{
	hadKeyDownEvent = true;
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

	// The renderer backend, and that is the whole of it. What used to be here
	// besides was save/restore of the fixed-function GL state Doom 3 left
	// behind - the ARB programs disabled, the array buffer unbound, every
	// texture unit turned off - because ImGui_ImplOpenGL2 drew through the same
	// context and would otherwise have inherited all of it. There is no context
	// to inherit here: every input to a draw is an argument to that draw.
	renderBackend->DrawImGui( ImGui::GetDrawData() );

	// reset this at the end of each frame, will be set again by the platform
	// backend's event handlers through NotifyKeyDownEvent()
	if ( hadKeyDownEvent ) {
		hadKeyDownEvent = false;
	}
}


void OpenWindow( D3ImGuiWindow win )
{
	if ( imguiCtx == NULL ) {
		// no context, so no window to open and nothing that could draw one.
		// SetNextWindowFocus() below dereferences the context unconditionally,
		// which is what made `dhewm3Settings` a crash rather than a no-op on a
		// build where Init() never ran.
		common->Printf( "The dhewm3 settings menu is not available: Dear ImGui "
		                "was not initialized.\n" );
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
