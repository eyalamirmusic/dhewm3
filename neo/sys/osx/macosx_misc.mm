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

#define GL_GLEXT_LEGACY // AppKit.h include pulls in gl.h already
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include "sys/platform.h"
#include "sys/sys_local.h"

/*
==================
idSysLocal::OpenURL
==================
*/
void idSysLocal::OpenURL( const char *url, bool doexit ) {
	static bool	quit_spamguard = false;

	if ( quit_spamguard ) {
		common->DPrintf( "Sys_OpenURL: already in a doexit sequence, ignoring %s\n", url );
		return;
	}

	common->Printf("Open URL: %s\n", url);


	[[ NSWorkspace sharedWorkspace] openURL: [ NSURL URLWithString:
		[ NSString stringWithCString: url encoding:NSUTF8StringEncoding ] ] ];

	if ( doexit ) {
		quit_spamguard = true;
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
	}
}

/*
==================
Sys_DoStartProcess
==================
*/
void Sys_DoStartProcess( const char *exeName, bool dofork = true ) {
	common->Printf( "TODO: Sys_DoStartProcess %s\n", exeName );
}

/*
==================
Sys_GetClipboardData
Sys_FreeClipboardData
Sys_SetClipboardData

The clipboard, which lived in posix_main.cpp on SDL_GetClipboardText until step
5. It is here rather than there because NSPasteboard needs Objective-C and this
is the port's one Objective-C++ file on the Sys_ side; posix_main.cpp is
deliberately window-system-free, and a pasteboard is a window system's.

Sys_GetClipboardData hands back a Mem_Alloc'd copy, which Sys_FreeClipboardData
frees. Returning NULL is normal and every caller checks: the pasteboard may hold
an image, or nothing at all.
==================
*/
char *Sys_GetClipboardData( void ) {
	NSPasteboard *	pb = [NSPasteboard generalPasteboard];
	NSString *		text = [pb stringForType:NSPasteboardTypeString];

	if ( text == nil ) {
		return NULL;
	}

	const char *	utf8 = [text UTF8String];
	if ( utf8 == NULL ) {
		return NULL;
	}

	size_t	len = strlen( utf8 );
	char *	data = (char *)Mem_Alloc( len + 1 );
	memcpy( data, utf8, len + 1 );

	return data;
}

void Sys_FreeClipboardData( char *data ) {
	Mem_Free( data );
}

void Sys_SetClipboardData( const char *string ) {
	NSPasteboard *	pb = [NSPasteboard generalPasteboard];

	[pb clearContents];
	[pb setString:[NSString stringWithCString:string encoding:NSUTF8StringEncoding]
		forType:NSPasteboardTypeString];
}

/*
==================
OSX_GetLocalizedString
==================
*/
const char* OSX_GetLocalizedString( const char* key )
{
	NSString *string = [ [ NSBundle mainBundle ] localizedStringForKey:[ NSString stringWithCString: key encoding:NSUTF8StringEncoding ]
													 value:@"No translation" table:nil];
	return [string UTF8String];
}
