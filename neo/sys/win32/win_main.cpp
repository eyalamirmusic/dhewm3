/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

/*
===========================================================================

The Windows Sys_ layer, on the eacp host.

This file used to be the host as well as the layer: a WinMain, a second
`while (1) common->Frame()`, an early console window with its own message pump,
seventeen wgl entry points for the MFC tools, and the Sys_ functions mixed in
among them. eacp owns the loop now (sys/eacp/Main.cpp -> Apps::run, and
sys/eacp/View.cpp for the frame inside it), so what is left here is the layer -
the same 30-odd entry points sys/posix/posix_main.cpp answers for the macOS
host, written against Win32 instead:

  paths and directory listings, the game DLLs, the clock, the clipboard, the
  console output and the two ways out of the process.

Three things went rather than moved, and are worth naming because they are
absences rather than omissions:

  - **The early console window.** win_syscon.cpp was a window that showed the
    log before the renderer came up and stayed for Sys_Error to write into.
    posix_main.cpp's Sys_ShowConsole is a no-op and always was; this one is too,
    and Sys_Error says what it has to say in a message box, because a
    /SUBSYSTEM:WINDOWS process has no console to have printed it to.
  - **The wgl pointers, Win_ChoosePixelFormat and Win_GetWindowScalingFactor.**
    All three were behind ID_ALLOW_TOOLS, which this buildsystem defines
    nowhere, and all three were OpenGL's or MFC's. There is no OpenGL on this
    build.
  - **setHighDPIMode.** eacp's own event loop calls
    SetProcessDpiAwarenessContext before a window exists, which is earlier than
    this file could.

===========================================================================
*/

#include "sys/platform.h"
#include "idlib/Str.h"
#include "idlib/containers/StrList.h"
#include "framework/Common.h"
#include "framework/CmdSystem.h"
#include "framework/FileSystem.h"
#include "framework/Licensee.h"
#include "sys/sys_local.h"

#include "sys/eacp/Platform.h"
#include "sys/win32/win_local.h"

// idlib/Str.h turns these four into macros - a compile error waiting in any
// standard or platform header included after it. posix_main.cpp does the same
// undef for the same reason; this file calls idStr::vsnPrintf explicitly where
// it wants idlib's.
#undef strcmp
#undef strncmp
#undef snprintf
#undef vsnprintf

#include <errno.h>
#include <float.h>
#include <fcntl.h>
#include <direct.h>
#include <io.h>
#include <time.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dbghelp.h>
#include <exception>
#include <sys/types.h>
#include <sys/stat.h>

// Was Win32Vars_t::win_outputDebugString, a static of a struct whose other
// members were the window handle, the module handle and the OS version - all
// three of which belonged to a host this file no longer is.
static idCVar win_outputDebugString( "win_outputDebugString", "0", CVAR_SYSTEM | CVAR_BOOL,
	"also send console output to the debugger's output window (OutputDebugString)" );

enum {
	MAXPRINTMSG = 4096
};

/*
===============
low level output

Sys_VPrintf is the one every other printf here goes through, which is the shape
posix_main.cpp has and the shape this file did not: upstream's Sys_Printf,
Sys_DebugPrintf and Sys_DebugVPrintf each formatted and wrote their own copy,
and Sys_VPrintf - which idlib and the console both call - had no Windows
definition at all.

stdout is dhewm3log.txt from Win_EarlyInit onwards, so this is the log as much
as it is the console.
===============
*/

void Sys_VPrintf( const char *msg, va_list arg ) {
	char text[MAXPRINTMSG];

	idStr::vsnPrintf( text, sizeof( text ) - 1, msg, arg );
	text[sizeof( text ) - 1] = '\0';

	fputs( text, stdout );

	if ( win_outputDebugString.GetBool() ) {
		OutputDebugString( text );
	}
}

void Sys_Printf( const char *msg, ... ) {
	va_list argptr;

	va_start( argptr, msg );
	Sys_VPrintf( msg, argptr );
	va_end( argptr );
}

void Sys_DebugPrintf( const char *fmt, ... ) {
	va_list argptr;

	va_start( argptr, fmt );
	Sys_VPrintf( fmt, argptr );
	va_end( argptr );
}

void Sys_DebugVPrintf( const char *fmt, va_list arg ) {
	Sys_VPrintf( fmt, arg );
}

/*
==============
Sys_ShowConsole

There is no console window to show. posix_main.cpp is the precedent - it has
answered the same nothing since the day it was written - and every caller
treats it as advisory.
==============
*/
void Sys_ShowConsole( int visLevel, bool quitOnClose ) {
}

/*
==============
Sys_ConsoleInput

The other half of the console that is not here. posix_main.cpp reads the
terminal the game was started from - tab completion, history, the lot - and a
windowed process started from Explorer has no terminal to read. The game's own
console, the one on the tilde key, is a different thing entirely and is
unaffected.

A dedicated server would want this back, and would want a console window with
it; neither host has one to give it today.
==============
*/
char *Sys_ConsoleInput( void ) {
	return NULL;
}

/*
=============
Sys_Error

The message box is not decoration. This is a windowed (/SUBSYSTEM:WINDOWS)
process whose stdout is a file, so a fatal error printed and exited would look
from the outside like the game vanishing on startup for no reason.
=============
*/
void Sys_Error( const char *error, ... ) {
	va_list	argptr;
	char	text[MAXPRINTMSG];

	va_start( argptr, error );
	idStr::vsnPrintf( text, sizeof( text ) - 1, error, argptr );
	va_end( argptr );
	text[sizeof( text ) - 1] = '\0';

	Sys_Printf( "Sys_Error: %s\n", text );
	fflush( stdout );

	timeEndPeriod( 1 );

	MessageBox( NULL, text, GAME_NAME " - fatal error",
	            MB_OK | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND );

	// exit() rather than ExitProcess(), so the atexit() that closes the log
	// still runs. Same reason Posix_Exit ends in exit().
	exit( EXIT_FAILURE );
}

/*
==============
Sys_Quit
==============
*/
void Sys_Quit( void ) {
	timeEndPeriod( 1 );
	exit( EXIT_SUCCESS );
}

/*
==============
Sys_Mkdir
==============
*/
void Sys_Mkdir( const char *path ) {
	_mkdir( path );
}

/*
=================
Sys_FileTimeStamp
=================
*/
ID_TIME_T Sys_FileTimeStamp( FILE *fp ) {
	struct _stat st;
	_fstat( _fileno( fp ), &st );
	return (long) st.st_mtime;
}

/*
==============
Sys_Cwd
==============
*/
const char *Sys_Cwd( void ) {
	static char cwd[MAX_OSPATH];

	_getcwd( cwd, sizeof( cwd ) - 1 );
	cwd[MAX_OSPATH-1] = 0;

	return cwd;
}

static int WPath2A(char *dst, size_t size, const WCHAR *src) {
	int len;
	BOOL default_char = FALSE;

	// test if we can convert lossless
	len = WideCharToMultiByte(CP_ACP, 0, src, -1, dst, size, NULL, &default_char);

	if (default_char) {
		/* The following lines implement a horrible
		   hack to connect the UTF-16 WinAPI to the
		   ASCII doom3 strings. While this should work in
		   most cases, it'll fail if the "Windows to
		   DOS filename translation" is switched off.
		   In that case the function will return NULL
		   and no homedir is used. */
		WCHAR w[MAX_OSPATH];
		len = GetShortPathNameW(src, w, sizeof(w));

		if (len == 0)
			return 0;

		/* Since the DOS path contains no UTF-16 characters, convert it to the system's default code page */
		len = WideCharToMultiByte(CP_ACP, 0, w, len, dst, size - 1, NULL, NULL);
	}

	if (len == 0)
		return 0;

	dst[len] = 0;
	/* Replace backslashes by slashes */
	for (int i = 0; i < len; ++i)
		if (dst[i] == '\\')
			dst[i] = '/';

	// cut trailing slash
	if (dst[len - 1] == '/') {
		dst[len - 1] = 0;
		len--;
	}

	return len;
}

/*
==============
Returns "My Documents"/My Games/dhewm3 directory (or equivalent - "CSIDL_PERSONAL").
To be used with Sys_GetPath(PATH_SAVE), so savegames, screenshots etc will be
saved to the users files instead of systemwide.

Based on (with kind permission) Yamagi Quake II's Sys_GetHomeDir()

Returns the number of characters written to dst
==============
 */
static int Win_GetHomeDir(char *dst, size_t size)
{
	int len;
	WCHAR profile[MAX_OSPATH];

	/* Get the path to "My Documents" directory */
	SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, profile);

	len = WPath2A(dst, size, profile);
	if (len == 0)
		return 0;

	idStr::Append(dst, size, "/My Games/dhewm3");

	return len;
}

static int GetRegistryPath(char *dst, size_t size, const WCHAR *subkey, const WCHAR *name) {
	WCHAR w[MAX_OSPATH];
	DWORD len = sizeof(w);
	HKEY res;
	DWORD sam = KEY_QUERY_VALUE
#ifdef _WIN64
		| KEY_WOW64_32KEY
#endif
		;
	DWORD type;

	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, sam, &res) != ERROR_SUCCESS)
		return 0;

	if (RegQueryValueExW(res, name, NULL, &type, (LPBYTE)w, &len) != ERROR_SUCCESS) {
		RegCloseKey(res);
		return 0;
	}

	RegCloseKey(res);

	if (type != REG_SZ)
		return 0;

	return WPath2A(dst, size, w);
}

bool Sys_GetPath(sysPath_t type, idStr &path) {
	char buf[MAX_OSPATH];
	struct _stat st;
	idStr s;

	switch(type) {
	case PATH_BASE:
		// try <path to exe>/base first
		if (Sys_GetPath(PATH_EXE, path)) {
			path.StripFilename();

			s = path;
			s.AppendPath(BASE_GAMEDIR);
			if (_stat(s.c_str(), &st) != -1 && (st.st_mode & _S_IFDIR)) {
				common->Warning("using path of executable: %s", path.c_str());
				return true;
			} else {
				s = path + "/demo/demo00.pk4";
				if (_stat(s.c_str(), &st) != -1 && (st.st_mode & _S_IFREG)) {
					common->Warning("using path of executable (seems to contain demo game data): %s ", path.c_str());
					return true;
				}
			}

			common->Warning("base path '%s' does not exist", s.c_str());
		}

		// Note: apparently there is no registry entry for the Doom 3 Demo

		// fallback to vanilla doom3 cd install
		if (GetRegistryPath(buf, sizeof(buf), L"SOFTWARE\\id\\Doom 3", L"InstallPath") > 0) {
			path = buf;
			return true;
		}

		// fallback to steam doom3 install
		if (GetRegistryPath(buf, sizeof(buf), L"SOFTWARE\\Valve\\Steam", L"InstallPath") > 0) {
			path = buf;
			path.AppendPath("steamapps\\common\\doom 3");

			if (_stat(path.c_str(), &st) != -1 && st.st_mode & _S_IFDIR)
				return true;
		}

		common->Warning("vanilla doom3 path not found either");

		return false;

	case PATH_CONFIG:
	case PATH_SAVE:
		if (Win_GetHomeDir(buf, sizeof(buf)) < 1) {
			Sys_Error("ERROR: Couldn't get dir to home path");
			return false;
		}

		path = buf;
		return true;

	case PATH_EXE:
		GetModuleFileName(NULL, buf, sizeof(buf) - 1);
		path = buf;
		path.BackSlashesToSlashes();
		return true;
	}

	return false;
}

/*
==============
Sys_ListFiles
==============
*/
int Sys_ListFiles( const char *directory, const char *extension, idStrList &list ) {
	idStr		search;
	struct _finddata_t findinfo;
	intptr_t	findhandle;
	int			flag;

	if ( !extension) {
		extension = "";
	}

	// passing a slash as extension will find directories
	if ( extension[0] == '/' && extension[1] == 0 ) {
		extension = "";
		flag = 0;
	} else {
		flag = _A_SUBDIR;
	}

	sprintf( search, "%s\\*%s", directory, extension );

	// search
	list.Clear();

	findhandle = _findfirst( search, &findinfo );
	if ( findhandle == -1 ) {
		return -1;
	}

	do {
		if ( flag ^ ( findinfo.attrib & _A_SUBDIR ) ) {
			list.Append( findinfo.name );
		}
	} while ( _findnext( findhandle, &findinfo ) != -1 );

	_findclose( findhandle );

	return list.Num();
}

/*
================
Sys_GetClipboardData
================
*/
char *Sys_GetClipboardData( void ) {
	char *data = NULL;
	char *cliptext;

	if ( OpenClipboard( NULL ) != 0 ) {
		HANDLE hClipboardData;

		if ( ( hClipboardData = GetClipboardData( CF_TEXT ) ) != 0 ) {
			if ( ( cliptext = (char *)GlobalLock( hClipboardData ) ) != 0 ) {
				data = (char *)Mem_Alloc( GlobalSize( hClipboardData ) + 1 );
				strcpy( data, cliptext );
				GlobalUnlock( hClipboardData );

				strtok( data, "\n\r\b" );
			}
		}
		CloseClipboard();
	}
	return data;
}

void Sys_FreeClipboardData( char* data ) {
	Mem_Free( data );
}

/*
================
Sys_SetClipboardData
================
*/
void Sys_SetClipboardData( const char *string ) {
	HGLOBAL HMem;
	char *PMem;

	// allocate memory block
	HMem = (char *)::GlobalAlloc( GMEM_MOVEABLE | GMEM_DDESHARE, strlen( string ) + 1 );
	if ( HMem == NULL ) {
		return;
	}
	// lock allocated memory and obtain a pointer
	PMem = (char *)::GlobalLock( HMem );
	if ( PMem == NULL ) {
		return;
	}
	// copy text into allocated memory block
	lstrcpy( PMem, string );
	// unlock allocated memory
	::GlobalUnlock( HMem );
	// open Clipboard
	if ( !OpenClipboard( 0 ) ) {
		::GlobalFree( HMem );
		return;
	}
	// remove current Clipboard contents
	EmptyClipboard();
	// supply the memory handle to the Clipboard
	SetClipboardData( CF_TEXT, HMem );
	HMem = 0;
	// close Clipboard
	CloseClipboard();
}

/*
========================================================================

DLL Loading

========================================================================
*/

/*
=====================
Sys_DLL_Load
=====================
*/
uintptr_t Sys_DLL_Load( const char *dllName ) {
	HINSTANCE	libHandle;
	libHandle = LoadLibrary( dllName );
	if ( libHandle ) {
		// since we can't have LoadLibrary load only from the specified path, check it did the right thing
		char loadedPath[ MAX_OSPATH ];
		GetModuleFileName( libHandle, loadedPath, sizeof( loadedPath ) - 1 );
		if ( idStr::IcmpPath( dllName, loadedPath ) ) {
			Sys_Printf( "ERROR: LoadLibrary '%s' wants to load '%s'\n", dllName, loadedPath );
			Sys_DLL_Unload( (uintptr_t)libHandle );
			return 0;
		}
	} else {
		DWORD e = GetLastError();

		if ( e ==  0x7E ) {
			// 0x7E is "The specified module could not be found."
			// don't print a warning for that error, it's expected
			// when trying different possible paths for a DLL
			return 0;
		}

		if ( e == 0xC1) {
			// "[193 (0xC1)] is not a valid Win32 application"
			// probably going to be common. Lets try to be less cryptic.
			common->Warning( "LoadLibrary( \"%s\" ) Failed ! [%i (0x%X)]\tprobably the DLL is of the wrong architecture, "
			                 "like x86_64 instead of arm64 (this build of dhewm3 expects %s)",
			                 dllName, e, e, D3_ARCH );
			return 0;
		}

		// for all other errors, print whatever FormatMessage() gives us
		LPVOID msgBuf = NULL;

		FormatMessage(
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL,
			e,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(LPTSTR)&msgBuf,
			0, NULL);

		common->Warning( "LoadLibrary( \"%s\" ) Failed ! [%i (0x%X)]\t%s", dllName, e, e, msgBuf );

		::LocalFree( msgBuf );
	}
	return (uintptr_t)libHandle;
}

/*
=====================
Sys_DLL_GetProcAddress
=====================
*/
void *Sys_DLL_GetProcAddress( uintptr_t dllHandle, const char *procName ) {
	void * adr = (void*)GetProcAddress((HINSTANCE)dllHandle, procName);
	if (!adr)
	{
		DWORD e = GetLastError();
		LPVOID msgBuf = NULL;

		FormatMessage(
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL,
			e,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(LPTSTR)&msgBuf,
			0, NULL);

		idStr errorStr = va("[%i (0x%X)]\t%s", e, e, msgBuf);

		if (errorStr.Length())
			common->Warning("GetProcAddress( %i %s) Failed ! %s", dllHandle, procName, errorStr.c_str());

		::LocalFree(msgBuf);
	}
	return adr;
}

/*
=====================
Sys_DLL_Unload
=====================
*/
void Sys_DLL_Unload( uintptr_t dllHandle ) {
	if ( !dllHandle ) {
		return;
	}
	if ( FreeLibrary( (HINSTANCE)dllHandle ) == 0 ) {
		int lastError = GetLastError();
		LPVOID lpMsgBuf;
		FormatMessage(
			FORMAT_MESSAGE_ALLOCATE_BUFFER,
		    NULL,
			lastError,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
			(LPTSTR) &lpMsgBuf,
			0,
			NULL
		);
		Sys_Error( "Sys_DLL_Unload: FreeLibrary failed - %s (%d)", lpMsgBuf, lastError );
	}
}

/*
================
Sys_Init

The cvar system must already be setup
================
*/
void Sys_Init( void ) {

	CoInitialize( NULL );

	// make sure the timer is high precision, otherwise
	// NT gets 18ms resolution
	timeBeginPeriod( 1 );

	{
		idStr savepath;
		Sys_GetPath( PATH_SAVE, savepath );
		common->Printf( "Logging console output to %s/dhewm3log.txt\n", savepath.c_str() );
	}

	// The Windows version check that stood here - GetVersionEx, "requires
	// Windows version 4 (NT) or greater", "doesn't run on Win32s" - is gone
	// along with the win32.osversion it filled in. GetVersionEx has reported
	// 6.2 for every unmanifested process since Windows 8.1, so it could not
	// have answered the question honestly; and the real floor for this build is
	// whatever D3D12 needs, which is far above anything it was testing for.

	common->Printf( "%d MB System Memory\n", Sys_GetSystemRam() );
}

/*
================
Sys_Shutdown
================
*/
void Sys_Shutdown( void ) {
	CoUninitialize();
}

// ---------- Time Stuff -------------

// D3_CpuPause() abstracts a CPU pause instruction, to make busy waits a bit less power-hungry
// (code taken from Yamagi Quake II)
#if defined(__GNUC__)
  #if (__i386 || __x86_64__)
    #define D3_CpuPause() asm volatile("pause")
  #elif defined(__aarch64__) || (defined(__ARM_ARCH) && __ARM_ARCH >= 7) || defined(__ARM_ARCH_6K__)
    #define D3_CpuPause() asm volatile("yield")
  #endif
#elif defined(_MSC_VER)
  #if defined(_M_IX86) || defined(_M_X64)
    #define D3_CpuPause() _mm_pause()
  #elif defined(_M_ARM) || defined(_M_ARM64)
    #define D3_CpuPause() __yield()
  #endif
#endif

#ifndef D3_CpuPause
  #warning "No D3_CpuPause implementation for this platform/architecture! Will busy-wait sometimes!"
#endif

static double perfCountToMS = 0.0; // set in Win_InitTime()
static LARGE_INTEGER firstCount = { 0 };

static size_t pauseLoopsPer5usec = 100; // set in Win_InitTime()

static void Win_InitTime() {
	LARGE_INTEGER freq = { 0 };
	QueryPerformanceFrequency(&freq); // in Hz
	perfCountToMS = 1000.0 / (double)freq.QuadPart; // 1/freq would be factor for seconds, we want milliseconds
	QueryPerformanceCounter(&firstCount);
	firstCount.QuadPart -= 1.5 / perfCountToMS; // make sure Sys_MillisecondsPrecise() always returns value >= 1

	double before = Sys_MillisecondsPrecise();
	for ( int i=0; i < 1000; ++i ) {
		// volatile so the call isn't optimized away
		volatile double x = Sys_MillisecondsPrecise();
		(void)x;
	}
	double after = Sys_MillisecondsPrecise();
	double callDiff = after - before;

#ifdef D3_CpuPause
	// figure out how long D3_CpuPause() instructions take
	before = Sys_MillisecondsPrecise();
	for( int i=0; i < 1000000; ++i ) {
		// call it 4 times per loop, so the ratio between pause and loop-instructions is better
		D3_CpuPause(); D3_CpuPause(); D3_CpuPause(); D3_CpuPause();
	}
	after = Sys_MillisecondsPrecise();
	double diff = after - before;
	double onePauseIterTime = diff / 1000000;
	if ( onePauseIterTime > 0.00000001 ) {
		double loopsPer10usec = 0.005 / onePauseIterTime;
		pauseLoopsPer5usec = loopsPer10usec;
		printf( "Win_InitTime(): A call to Sys_MillisecondsPrecise() takes about %g nsec; 1mio pause loops took %g ms => pauseLoopsPer5usec = %zd\n",
		        callDiff*1000.0, diff, pauseLoopsPer5usec );
		if ( pauseLoopsPer5usec == 0 )
			pauseLoopsPer5usec = 1;
	} else {
		assert( 0 && "apparently 1mio pause loops are so fast we can't even measure it?!" );
		pauseLoopsPer5usec = 1000000;
	}
	// Note: Due to CPU frequency scaling this is not super precise, but it should be within
	//   an order of magnitude of the real current value, I think, which should suffice for our purposes
#else
	printf( "Win_InitTime(): A call to Sys_MillisecondsPrecise() takes about %g nsecs\n", callDiff*1000.0 );
#endif
}

/*
=======================
Sys_MillisecondsPrecise
=======================
*/
double Sys_MillisecondsPrecise() {
	LARGE_INTEGER cur;
	QueryPerformanceCounter(&cur);

	double ret = cur.QuadPart - firstCount.QuadPart;
	ret *= perfCountToMS;
	return ret;
}

/*
=====================
Sys_SleepUntilPrecise
=====================
*/
void Sys_SleepUntilPrecise( double targetTimeMS ) {
	double msec = targetTimeMS - Sys_MillisecondsPrecise();
	if ( msec < 0.01 ) // don't bother for less than 10usec
		return;

	if ( msec > 2.0 ) {
		// Note: Theoretically one could use SetWaitableTimer() and WaitForSingleObject()
		//   for higher precision, but last time I tested (on Win10),
		//   in practice that also only had millisecond-precision
		dword sleepMS = msec - 1.0; // wait for last MS or so in busy(-ish) loop below
		Sleep( sleepMS );
	}

	// wait for the remaining time with a busy loop, as that has higher precision
	do {
#ifdef D3_CpuPause
		for ( size_t i=0; i < pauseLoopsPer5usec; ++i ) {
			// call it 4 times per loop, so the ratio between pause and loop-instructions is better
			D3_CpuPause(); D3_CpuPause(); D3_CpuPause(); D3_CpuPause();
		}
#endif

		msec = targetTimeMS - Sys_MillisecondsPrecise();
	} while ( msec >= 0.01 );
}

/*
========================================================================

stdout/stderr redirection, originally from SDL_win32_main.c

This is what makes Sys_VPrintf above a log rather than a write to a handle
nobody holds. It goes to Documents/My Games/dhewm3 rather than next to the
executable, which may well not be writable.

========================================================================
*/

#define STDOUT_FILE	TEXT("dhewm3log.txt")
#define STDERR_FILE	TEXT("stderr.txt")

/* Set a variable to tell if the stdio redirect has been enabled. */
static int stdioRedirectEnabled = 0;
static char stdoutPath[MAX_PATH];
static char stderrPath[MAX_PATH];
#define DIR_SEPERATOR TEXT("/")

/* Remove the output files if there was no output written */
static void cleanup_output(void) {
	FILE *file;
	int empty;

	/* Flush the output in case anything is queued */
	fclose(stdout);
	fclose(stderr);

	/* Without redirection we're done */
	if (!stdioRedirectEnabled) {
		return;
	}

	/* See if the files have any output in them */
	if ( stdoutPath[0] ) {
		file = fopen(stdoutPath, TEXT("rb"));
		if ( file ) {
			empty = (fgetc(file) == EOF) ? 1 : 0;
			fclose(file);
			if ( empty ) {
				remove(stdoutPath);
			}
		}
	}
	if ( stderrPath[0] ) {
		file = fopen(stderrPath, TEXT("rb"));
		if ( file ) {
			empty = (fgetc(file) == EOF) ? 1 : 0;
			fclose(file);
			if ( empty ) {
				remove(stderrPath);
			}
		}
	}
}

/* Redirect the output (stdout and stderr) to a file */
static void redirect_output(void)
{
	char path[MAX_PATH];
	struct _stat st;

	/* DG: use "My Documents/My Games/dhewm3" to write stdout.txt and stderr.txt
	*     instead of the binary, which might not be writable */
	Win_GetHomeDir(path, sizeof(path));

	if (_stat(path, &st) == -1) {
		/* oops, "My Documents/My Games/dhewm3" doesn't exist - does My Games/ at least exist? */
		char myGamesPath[MAX_PATH];
		char* lastslash;
		memcpy(myGamesPath, path, MAX_PATH);
		lastslash = strrchr(myGamesPath, '/');
		if (lastslash != NULL) {
			*lastslash = '\0';
		}
		if (_stat(myGamesPath, &st) == -1) {
			/* if My Documents/My Games/ doesn't exist, create it */
			if( _mkdir(myGamesPath) != 0 && errno != EEXIST ) {
				char msg[2048];
				D3_snprintfC99( msg, sizeof(msg), "Failed to create '%s',\n error number is %d (%s).\nPermission problem?",
				                myGamesPath, errno, strerror(errno) );
				MessageBox( NULL, msg, "Can't create 'My Games' directory!", MB_OK | MB_ICONERROR );
				exit(1);
			}
		}
		/* create My Documents/My Games/dhewm3/ */
		if( _mkdir(path) != 0 && errno != EEXIST ) {
			char msg[2048];
			D3_snprintfC99( msg, sizeof(msg), "Failed to create '%s'\n(for savegames, configs and logs),\n error number is %d (%s)\nIs Documents/My Games/ write protected?",
			                path, errno, strerror(errno) );
			MessageBox( NULL, msg, "Can't create 'My Games/dhewm3' directory!", MB_OK | MB_ICONERROR );
			exit(1);
		}
	}

	FILE *newfp;

	idStr::Copynz( stdoutPath, path, sizeof(stdoutPath) );
	idStr::Append( stdoutPath, sizeof(stdoutPath), DIR_SEPERATOR STDOUT_FILE );

	{ /* DG: rename old stdout log */
		char stdoutPathBK[MAX_PATH];
		idStr::Copynz( stdoutPathBK, path, sizeof(stdoutPathBK) );
		idStr::Append( stdoutPathBK, sizeof(stdoutPathBK), DIR_SEPERATOR TEXT("dhewm3log-old.txt") );
		rename( stdoutPath, stdoutPathBK );
	} /* DG end */

	  /* Redirect standard input and standard output */
	newfp = freopen(stdoutPath, TEXT("w"), stdout);

	if ( newfp == NULL ) {	/* This happens on NT */
#if !defined(stdout)
		stdout = fopen(stdoutPath, TEXT("w"));
#else
		newfp = fopen(stdoutPath, TEXT("w"));
		if ( newfp ) {
			*stdout = *newfp;
		} else {
			char msg[2048];
			D3_snprintfC99( msg, sizeof(msg), "Failed to create '%s',\n error number is %d (%s)\nIs Documents/My Games/dhewm3/\n or dhewm3log.txt write protected?",
			                stdoutPath, errno, strerror(errno) );
			MessageBox( NULL, msg, "Can't create dhewm3log.txt!", MB_OK | MB_ICONERROR );
			exit(1);
		}
#endif
	}

	idStr::Copynz( stderrPath, path, sizeof(stderrPath) );
	idStr::Append( stderrPath, sizeof(stderrPath), DIR_SEPERATOR STDERR_FILE );

	newfp = freopen(stderrPath, TEXT("w"), stderr);
	if ( newfp == NULL ) {	/* This happens on NT */
#if !defined(stderr)
		stderr = fopen(stderrPath, TEXT("w"));
#else
		newfp = fopen(stderrPath, TEXT("w"));
		if ( newfp ) {
			*stderr = *newfp;
		} else {
			char msg[2048];
			D3_snprintfC99( msg, sizeof(msg), "Failed to create '%s',\n error number is %d (%s)\nIs Documents/My Games/dhewm3/ write protected?",
			                stderrPath, errno, strerror(errno) );
			MessageBox( NULL, msg, "Can't create stderr.txt!", MB_OK | MB_ICONERROR );
			exit(1);
		}
#endif
	}

	setvbuf(stdout, NULL, _IOLBF, BUFSIZ);	/* Line buffered */
	setbuf(stderr, NULL);			/* No buffering */
	stdioRedirectEnabled = 1;
}

/*
========================================================================

The crash handler, which is what posix_main.cpp's Posix_InitSignalHandlers is
for on the other host: something went wrong in a way the engine cannot report,
so say where before the process goes.

It has to catch three different ways of dying, because Windows does not route
them through one place the way a signal does:

  - **A structured exception** - an access violation, a divide by zero, an
    illegal instruction. SetUnhandledExceptionFilter sees these.
  - **std::terminate**, which is where an uncaught C++ exception ends up. eacp
    is C++/WinRT underneath, whose check_hresult throws, so a D3D12 call that
    fails in a place nothing catches arrives here rather than at the filter.
  - **The CRT's invalid parameter handler**, which is what fputs to a closed
    FILE, a bad printf conversion or a fclose of a handle already closed reach.
    Its default is __fastfail, and __fastfail is invisible to everything above:
    it does not raise, so the filter never runs and the process dies with
    STATUS_STACK_BUFFER_OVERRUN (0xC0000409) and no explanation at all. That is
    the one this was written for.

The backtrace is CaptureStackBackTrace rather than StackWalk64, and the choice
is worth a line: all three of these run on the failing thread with its frames
still on the stack, so walking up from here walks through them, and it costs no
CONTEXT plumbing and no per-architecture machine types. Symbol names need a PDB
- the Release configuration does not build one, so a Release crash gives module
names and offsets, which still says which library it was in.

========================================================================
*/

static void CrashPrintf( const char *fmt, ... ) {
	char	text[MAXPRINTMSG];
	va_list	argptr;

	va_start( argptr, fmt );
	idStr::vsnPrintf( text, sizeof( text ) - 1, fmt, argptr );
	va_end( argptr );
	text[sizeof( text ) - 1] = '\0';

	// Not Sys_Printf: this runs when the process is already broken, and one of
	// the ways it gets here is stdout itself being unusable. Both sinks are
	// tried, and the debugger's cannot fail.
	OutputDebugString( text );
	fputs( text, stdout );
	fflush( stdout );
}

static void CrashBacktrace( const char *what ) {
	static LONG	entered = 0;

	// Once. A handler that faults inside itself would otherwise recurse until
	// the stack runs out, and the second report would be about the handler.
	if ( InterlockedExchange( &entered, 1 ) != 0 ) {
		return;
	}

	CrashPrintf( "\n=== %s ===\n", what );

	void *	frames[62];
	USHORT	count = CaptureStackBackTrace( 0, 62, frames, NULL );

	HANDLE	process = GetCurrentProcess();

	SymSetOptions( SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME );
	SymInitialize( process, NULL, TRUE );

	// SYMBOL_INFO carries the name past the end of the struct, so it is sized
	// rather than declared.
	char			buffer[sizeof( SYMBOL_INFO ) + MAX_SYM_NAME];
	SYMBOL_INFO *	symbol = (SYMBOL_INFO *)buffer;

	memset( buffer, 0, sizeof( buffer ) );
	symbol->SizeOfStruct = sizeof( SYMBOL_INFO );
	symbol->MaxNameLen = MAX_SYM_NAME;

	for ( USHORT i = 0; i < count; ++i ) {
		DWORD64	address = (DWORD64)frames[i];
		DWORD64	displacement = 0;

		if ( SymFromAddr( process, address, &displacement, symbol ) ) {
			IMAGEHLP_LINE64	line = {};
			DWORD			lineDisplacement = 0;

			line.SizeOfStruct = sizeof( line );

			if ( SymGetLineFromAddr64( process, address, &lineDisplacement, &line ) ) {
				CrashPrintf( "  #%-2d %s  (%s:%lu)\n", i, symbol->Name,
				             line.FileName, line.LineNumber );
			} else {
				CrashPrintf( "  #%-2d %s + 0x%llx\n", i, symbol->Name, displacement );
			}

			continue;
		}

		// No PDB, which is the Release configuration's normal case. The module
		// and the offset into it are still worth having.
		char		moduleName[MAX_PATH] = "?";
		HMODULE		module = NULL;

		if ( GetModuleHandleEx( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
		                        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                        (LPCSTR)frames[i], &module ) ) {
			GetModuleFileName( module, moduleName, sizeof( moduleName ) );
			CrashPrintf( "  #%-2d %s + 0x%llx\n", i, moduleName,
			             address - (DWORD64)module );
			continue;
		}

		CrashPrintf( "  #%-2d 0x%llx\n", i, address );
	}

	CrashPrintf( "=== end of backtrace ===\n" );
}

static LONG WINAPI Win_ExceptionFilter( EXCEPTION_POINTERS *pointers ) {
	const EXCEPTION_RECORD *	record = pointers->ExceptionRecord;

	CrashBacktrace( va( "unhandled exception 0x%08lx at 0x%p",
	                    record->ExceptionCode, record->ExceptionAddress ) );

	return EXCEPTION_EXECUTE_HANDLER;	// die, but having said why
}

static void Win_TerminateHandler() {
	// What the exception was, if there is one in flight - which there is for
	// every way of reaching std::terminate that this cares about.
	const char *	what = "std::terminate";

	if ( std::current_exception() ) {
		try {
			std::rethrow_exception( std::current_exception() );
		} catch ( const std::exception &e ) {
			what = va( "std::terminate: %s", e.what() );
		} catch ( ... ) {
			what = "std::terminate: a non-std exception";
		}
	}

	CrashBacktrace( what );
	_exit( 3 );
}

static void Win_InvalidParameterHandler( const wchar_t *expression, const wchar_t *function,
                                         const wchar_t *file, unsigned int line, uintptr_t ) {
	// The four arguments are only filled in by the debug CRT; the release one
	// passes null for all of them, which is exactly when this is hardest to
	// read without the backtrace below.
	CrashBacktrace( va( "CRT invalid parameter in %ls (%ls:%u): %ls",
	                    function ? function : L"?",
	                    file ? file : L"?", line,
	                    expression ? expression : L"?" ) );
	_exit( 3 );
}

static void Win_InstallCrashHandlers() {
	SetUnhandledExceptionFilter( Win_ExceptionFilter );
	std::set_terminate( Win_TerminateHandler );
	_set_invalid_parameter_handler( Win_InvalidParameterHandler );

	// abort() otherwise puts up a dialog nobody is there to dismiss, and this
	// is a windowed process that may be running under a script.
	_set_abort_behavior( 0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT );
}

/*
==================
Win_EarlyInit

Everything that has to be true before eacp opens a window, which is everything
the first printed line and the first read clock depend on. sys/eacp/Main.cpp
calls this where the macOS host calls Sys_InitPaths + Posix_InitSignalHandlers,
and for the same reason: Posix_InitSignalHandlers opens that host's log and
starts its clock too.

There is no Windows Sys_InitPaths beside it, because there is nothing to
resolve: Sys_GetPath answers from GetModuleFileName and the registry every time
it is asked, and unlike the macOS host this one does not have to make any
directory current for its own game data to be findable.
==================
*/
void Win_EarlyInit( void ) {
	// as the very first thing, redirect stdout to dhewm3log.txt (and stderr to
	// stderr.txt) so we can log
	redirect_output();
	atexit(cleanup_output);

	// now that stdout is redirected to dhewm3log.txt,
	// log its (approx.) creation time before anything else is logged:
	{
		time_t tt = time(NULL);
		const struct tm* tms = localtime(&tt);
		char timeStr[64] = {};
		strftime(timeStr, sizeof(timeStr), "%F %H:%M:%S", tms);
		printf("Opened this log at %s\n", timeStr);
	}

	// the base time for Sys_MillisecondsPrecise() should be set very early
	Win_InitTime();

	Sys_SetPhysicalWorkMemory( 192 << 20, 1024 << 20 );

	// no abort/retry/fail errors
	SetErrorMode( SEM_FAILCRITICALERRORS );

	// Last, because it is the thing that reports on everything above it having
	// gone wrong, and because it wants the log open to report into.
	Win_InstallCrashHandlers();
}

/*
==================
idSysLocal::OpenURL
==================
*/
void idSysLocal::OpenURL( const char *url, bool doexit ) {
	static bool doexit_spamguard = false;
	HWND wnd;

	if (doexit_spamguard) {
		common->DPrintf( "OpenURL: already in an exit sequence, ignoring %s\n", url );
		return;
	}

	common->Printf("Open URL: %s\n", url);

	if ( !ShellExecute( NULL, "open", url, NULL, NULL, SW_RESTORE ) ) {
		common->Error( "Could not open url: '%s' ", url );
		return;
	}

	wnd = GetForegroundWindow();
	if ( wnd ) {
		ShowWindow( wnd, SW_MAXIMIZE );
	}

	if ( doexit ) {
		doexit_spamguard = true;
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
	}
}

/*
==================
idSysLocal::StartProcess
==================
*/
void idSysLocal::StartProcess( const char *exePath, bool doexit ) {
	TCHAR				szPathOrig[_MAX_PATH];
	STARTUPINFO			si;
	PROCESS_INFORMATION	pi;

	ZeroMemory( &si, sizeof(si) );
	si.cb = sizeof(si);

	strncpy( szPathOrig, exePath, _MAX_PATH );
	szPathOrig[_MAX_PATH-1] = 0;

	if( !CreateProcess( NULL, szPathOrig, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ) ) {
		common->Error( "Could not start process: '%s' ", szPathOrig );
	    return;
	}

	if ( doexit ) {
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
	}
}
