/*
===========================================================================

dhewm 3 on eacp - the event queue, with only the console half filled in.

sys/events.cpp is 1928 lines of SDL and this build does not compile it. What
replaces it arrives in two pieces: this one, which is the queue itself and the
one producer that needs no window - the terminal - and Phase 2 step 3, which
adds the keyboard and the mouse by pushing eacp's callbacks into the same
queue.

The queue is here rather than in the view because that is where it already was:
sys/events.cpp:909 pushes console lines into SDL's event queue as SDL_USEREVENT
and reads them back out in Sys_GetEvent, because the engine polls for events
and the terminal reader hands them over. eacp's keyboard and mouse arrive as
callbacks on the main thread and want the same treatment, so the ring outlives
the console it was written for.

===========================================================================
*/

#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/CmdSystem.h"
#include "framework/KeyInput.h"
#include "renderer/tr_local.h"
#include "sys/sys_public.h"

#include "idlib/containers/List.h"

/*
================================================================================

	the queue

================================================================================
*/

static const sysEvent_t no_event = { SE_NONE, 0, 0, 0, NULL };

// Produced and consumed on the main thread only: eacp delivers input on the
// loop thread and idCommonLocal::Frame runs there too, so this needs no lock.
// Sys_ConsoleInput is the one exception and it is not asynchronous either - the
// terminal is read by Sys_GenerateEvents, on this thread, once a frame.
static idList<sysEvent_t> eventQueue;

static void PushEvent( const sysEvent_t &ev ) {
	eventQueue.Append( ev );
}

static void PushConsoleEvent( const char *s ) {
	size_t len = strlen( s ) + 1;
	char *b = (char *)Mem_Alloc( len );
	strcpy( b, s );

	sysEvent_t ev = { };
	ev.evType = SE_CONSOLE;
	ev.evPtrLength = (int)len;
	ev.evPtr = b;

	PushEvent( ev );
}

/*
================
Sys_GetEvent
================
*/
sysEvent_t Sys_GetEvent() {
	if ( eventQueue.Num() == 0 ) {
		return no_event;
	}

	sysEvent_t ev = eventQueue[0];
	eventQueue.RemoveIndex( 0 );
	return ev;
}

/*
================
Sys_ClearEvents
================
*/
void Sys_ClearEvents() {
	// The engine frees evPtr on the events it is handed; the ones dropped here
	// were never handed over, so this is the only place that owns them.
	for ( int i = 0; i < eventQueue.Num(); i++ ) {
		if ( eventQueue[i].evPtr != NULL ) {
			Mem_Free( eventQueue[i].evPtr );
		}
	}

	eventQueue.Clear();
}

/*
================
Sys_GenerateEvents
================
*/
void Sys_GenerateEvents() {
	char *s = Sys_ConsoleInput();

	if ( s ) {
		PushConsoleEvent( s );
	}

	// Nothing to pump: eacp's loop delivers input by callback, and the callbacks
	// run on this thread between frames rather than being drained here.
}

/*
================================================================================

	input devices - Phase 2 step 3

	Everything below is the shape sys/events.cpp has, answering "no device" until
	the view starts feeding the queue above. The engine calls all of it every
	frame regardless of whether anything is bound, so none of these may be left
	undefined.

================================================================================
*/

// Read by idUsercmdGenLocal::MakeCurrent (framework/UsercmdGen.cpp:922) to know
// whether a cursor GUI is swallowing the gamepad's face buttons. There is no
// gamepad here yet, so it never becomes true - but it is not the gamepad's
// variable, it is the game's, and the game reads it every frame.
bool D3_IN_interactiveIngameGuiActive = false;

// Both of these are the settings menu's, not the input layer's: the menu names
// the pad's start button and offers the layout as an option. They stay with the
// input layer because that is where the pad will be.
idCVar joy_gamepadLayout( "joy_gamepadLayout", "-1", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_NOCHEAT | CVAR_INTEGER,
		"Button layout of gamepad. -1: auto, 0: XBox-style, 1: Nintendo-style, 2: PS4/5-style, 3: PS2/3-style",
		idCmdSystem::ArgCompletion_Integer<-1, 3> );

const char* D3_GetGamepadStartButtonName() {
	return "Pad Start";
}

void Sys_InitInput() {
}

void Sys_ShutdownInput() {
	Sys_ClearEvents();
}

void Sys_InitScanTable() {
}

unsigned char Sys_GetConsoleKey( bool shifted ) {
	return shifted ? '~' : '`';
}

unsigned char Sys_MapCharForKey( int key ) {
	return (unsigned char)( key & 0xff );
}

const char* Sys_GetScancodeName( int key ) {
	return NULL;
}

const char* Sys_GetLocalizedScancodeName( int key ) {
	return Sys_GetScancodeName( key );
}

const char* Sys_GetLocalizedScancodeNameUTF8( int key ) {
	return Sys_GetScancodeName( key );
}

int Sys_GetKeynumForScancodeName( const char* name ) {
	return 0;
}

const char* Sys_GetLocalizedJoyKeyName( int key ) {
	return NULL;
}

int Sys_PollKeyboardInputEvents() {
	return 0;
}

int Sys_ReturnKeyboardInputEvent( const int n, int &key, bool &state ) {
	return 0;
}

void Sys_EndKeyboardInputEvents() {
}

int Sys_PollMouseInputEvents() {
	return 0;
}

int Sys_ReturnMouseInputEvent( const int n, int &action, int &value ) {
	return 0;
}

void Sys_EndMouseInputEvents() {
}

void Sys_SetRumble( int device, int low, int hi ) {
}

int Sys_PollJoystickInputEvents( int deviceNum ) {
	return 0;
}

int Sys_ReturnJoystickInputEvent( const int n, int &action, int &value ) {
	return 0;
}

void Sys_EndJoystickInputEvents() {
}

void Sys_GrabMouseCursor( bool grabIt ) {
}

void Sys_SetInteractiveIngameGuiActive( bool active, idUserInterface* ui ) {
}
