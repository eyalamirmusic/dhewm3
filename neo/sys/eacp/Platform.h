/*
===========================================================================

dhewm 3 on eacp - the parts of the platform that are neither the loop nor a
window: where the game's files live, and how much memory the machine has.

sys/osx/DOOMController.mm holds these for the SDL/GL build, wrapped around an
SDL_main that opens the game's own loop. This file is that file with the loop
taken out - Apps::run owns it here (plan.md, Phase 2 step 2).

===========================================================================
*/

#pragma once

// Resolves PATH_EXE / PATH_BASE / PATH_SAVE and makes the bundle's Resources
// directory current. Call before anything reads a path - Posix_InitSignalHandlers
// writes its crash log next to the executable, and the file system is built on
// PATH_BASE.
void Sys_InitPaths( void );
