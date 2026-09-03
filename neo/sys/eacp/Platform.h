/*
===========================================================================

dhewm 3 on eacp - the parts of the platform that are neither the loop nor a
window: where the game's files live, and how much memory the machine has.

sys/osx/DOOMController.mm held these for the SDL/GL build, wrapped around an
SDL_main that opened the game's own loop. This file is that file with the loop
taken out - Apps::run owns it here (plan.md, Phase 2 step 2).

===========================================================================
*/

#pragma once

// Both hosts have one call that has to happen before Apps::run opens a window,
// and they are not the same call, because what is unresolved before it differs.

#ifdef _WIN32

// Opens Documents/My Games/dhewm3/dhewm3log.txt and points stdout at it, and
// starts the clock Sys_MillisecondsPrecise reads. Defined in
// sys/win32/win_main.cpp, which says why there is no Sys_InitPaths beside it.
void Win_EarlyInit( void );

#else

// Resolves PATH_EXE / PATH_BASE / PATH_SAVE and makes the bundle's Resources
// directory current. Call before anything reads a path - Posix_InitSignalHandlers
// writes its crash log next to the executable, and the file system is built on
// PATH_BASE.
void Sys_InitPaths( void );

#endif
