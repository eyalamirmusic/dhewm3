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

#ifndef __WIN_LOCAL_H__
#define __WIN_LOCAL_H__

/*
What every sys/win32/ file needs before it can say anything: the Windows
headers, in the order winsock2 has to come first in, and the two engine headers
they are all written against.

It used to declare rather more - Sys_CreateConsole/Sys_DestroyConsole and
Conbuf_AppendText for the early console window, Win_GetScanTable/Win_MapKey for
the DirectInput layer, Win_SetErrorText, and the Win32Vars_t that held the
window handle, the module handle and the OS version. eacp owns the window and
the input on this host, so none of the three files below has a use for any of
it; win_main.cpp says which of them went and why.
*/

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <objbase.h>

#include "framework/CVarSystem.h"
#include "sys/sys_public.h"

#endif /* !__WIN_LOCAL_H__ */
