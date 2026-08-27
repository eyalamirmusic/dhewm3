/*
===========================================================================

dhewm 3 on eacp - paths, memory size, and the machine-age question.

Lifted from sys/osx/DOOMController.mm, which the eacp target does not build:
that file's SDL_main is a second entry point and a second main loop, and this
target has Apps::run for both. What is left is the part that was never about
SDL at all.

===========================================================================
*/

#include "Platform.h"

#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#import <Foundation/Foundation.h>

#include "sys/platform.h"
#include "idlib/Str.h"
#include "framework/Common.h"
#include "sys/posix/posix_public.h"

static char base_path[MAXPATHLEN];
static char exe_path[MAXPATHLEN];
static char save_path[MAXPATHLEN];

const char* Posix_GetExePath() {
	return exe_path;
}

const char* Posix_GetSavePath() {
	return save_path;
}

bool Sys_GetPath(sysPath_t type, idStr &path) {
	switch(type) {
	case PATH_BASE:
		path = base_path;
		return true;

	case PATH_CONFIG:
	case PATH_SAVE:
		path = save_path;
		return true;

	case PATH_EXE:
		path = exe_path;
		return true;
	}

	return false;
}

void Sys_InitPaths( void ) {
	NSBundle *bundle = [NSBundle mainBundle];

	idStr::Copynz(exe_path, [[bundle bundlePath] cStringUsingEncoding:NSUTF8StringEncoding], sizeof(exe_path));

	D3_snprintfC99(save_path, sizeof(save_path), "%s/Library/Application Support/dhewm3",
	               [NSHomeDirectory() cStringUsingEncoding:NSUTF8StringEncoding]);

	// The directory the .app sits in, which is where a build's game libraries
	// and its unpacked demo data are.
	idStr::Copynz(base_path, exe_path, sizeof(base_path));
	char *lastSlash = strrchr(base_path, '/');
	if (lastSlash)
		*lastSlash = '\0';

	// DOOMController.mm makes the bundle's Resources directory current instead,
	// which is where Doom3.app used to keep its game data. Nothing in the engine
	// reads the working directory - Posix_Cwd has no callers - so all it decides
	// is where a relative path typed at the console lands, and PATH_BASE is the
	// directory that has the game data in it. This bundle has no Resources at
	// all, so the old line would have been a fatal error on an empty question.
	if (![[NSFileManager defaultManager] changeCurrentDirectoryPath:
			[NSString stringWithUTF8String:base_path]])
		Sys_Error("Could not change to the directory holding %s", exe_path);
}

/*
===============
Sys_Shutdown
===============
*/
void Sys_Shutdown( void ) {
	Posix_Shutdown();
}

/*
================
Sys_GetSystemRam
returns in megabytes
================
*/
int Sys_GetSystemRam( void ) {
	int mib[2] = { CTL_HW, HW_MEMSIZE };
	uint64_t memsize = 0;
	size_t len = sizeof(memsize);

	if (sysctl(mib, 2, &memsize, &len, NULL, 0) == 0)
		return (int)(memsize / (1024*1024));

	return 1024;
}

/*
================
OSX_GetCPUIdentification

Com_ExecMachineSpec_f asks this one question of it: whether the machine is old
enough that stencil shadows should default off. DOOMController.mm answers with
Gestalt(gestaltNativeCPUtype), whose entire purpose is to tell a PowerPC G4
from a G5 - the answer has been "not old" on every Mac since 2005, and this
build needs Metal, so it is "not old" by construction.
================
*/
bool OSX_GetCPUIdentification( int& cpuId, bool& oldArchitecture )
{
	cpuId = 0;
	oldArchitecture = false;
	return true;
}
