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

// The threading half of the platform layer, on the standard library rather
// than on SDL. Nothing here needs a window system, so it is the one piece of
// the eacp port (plan.md, Phase 2) that both executables can share unchanged -
// and the only piece the Phase 1 regression gate can measure.

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "sys/platform.h"
#include "framework/Common.h"

#include "sys/sys_public.h"

// What xthreadInfo::threadHandle points at. Declared but never defined in
// sys_public.h, so this is the only place its shape is known.
struct sysThread_t {
	std::thread		thread;
	std::thread::id	id;
};

// Recursive, and it has to be: the engine re-enters a critical section it
// already holds. idSoundWorldLocal::ProcessDemoCommand takes
// CRITICAL_SECTION_ZERO around ReadFromSaveGame, whose first act is
// ClearAllSoundEmitters, which takes it again (snd_world.cpp:340 and :203).
// That was never a bug: both implementations this API has ever had are
// recursive - Win32's CRITICAL_SECTION by definition, and SDL_CreateMutex,
// which documents itself as reentrant. A plain std::mutex deadlocks there, and
// nothing about it fails to compile.
//
// condition_variable_any rather than condition_variable follows from it:
// the latter only waits on a unique_lock<mutex>.
static std::recursive_mutex			mutex[MAX_CRITICAL_SECTIONS];
static std::condition_variable_any	cond[MAX_TRIGGER_EVENTS];
static bool						signaled[MAX_TRIGGER_EVENTS] = { };
static bool						waiting[MAX_TRIGGER_EVENTS] = { };

static xthreadInfo	*thread[MAX_THREADS] = { };
static size_t		thread_count = 0;

static bool				mainThreadIDset = false;
static std::thread::id	mainThreadID;

// xthreadInfo::threadId is part of a public struct and nothing outside this
// file reads it, but it is kept filled in because a thread id is the sort of
// thing a debugger session wants to see. std::thread::id has no numeric value
// of its own, so this is the only portable way to get one.
static uint64_t numericThreadID( std::thread::id id ) {
	return (uint64_t)std::hash<std::thread::id>()( id );
}

/*
==============
Sys_Sleep
==============
*/
void Sys_Sleep(int msec) {
	std::this_thread::sleep_for( std::chrono::milliseconds( msec ) );
}

/*
==================
Sys_InitThreads
==================
*/
void Sys_InitThreads() {
	mainThreadID = std::this_thread::get_id();
	mainThreadIDset = true;

	// The mutexes and condition variables are objects with lifetimes of their
	// own now, rather than handles that had to be created and destroyed - so
	// there is nothing left to do for either. What remains is the state the
	// trigger events carry on top of them.
	for (int i = 0; i < MAX_TRIGGER_EVENTS; i++) {
		signaled[i] = false;
		waiting[i] = false;
	}

	for (int i = 0; i < MAX_THREADS; i++)
		thread[i] = NULL;

	thread_count = 0;
}

/*
==================
Sys_ShutdownThreads
==================
*/
void Sys_ShutdownThreads() {
	for (size_t i = 0; i < thread_count; i++) {
		if (!thread[i])
			continue;

		Sys_Printf("WARNING: Thread '%s' still running\n", thread[i]->name);

		// There is no killing a std::thread, exactly as there was none in
		// SDL2, so the thread is left to run. It has to be *detached* before
		// its handle goes away though: destroying a joinable std::thread calls
		// std::terminate, which would turn this warning into a crash on the
		// way out.
		sysThread_t *handle = thread[i]->threadHandle;
		if (handle) {
			handle->thread.detach();
			delete handle;
			thread[i]->threadHandle = NULL;
		}

		thread[i] = NULL;
	}

	for (int i = 0; i < MAX_TRIGGER_EVENTS; i++) {
		signaled[i] = false;
		waiting[i] = false;
	}

	thread_count = 0;
}

/*
==================
Sys_EnterCriticalSection
==================
*/
void Sys_EnterCriticalSection(int index) {
	assert(index >= 0 && index < MAX_CRITICAL_SECTIONS);

	mutex[index].lock();
}

/*
==================
Sys_LeaveCriticalSection
==================
*/
void Sys_LeaveCriticalSection(int index) {
	assert(index >= 0 && index < MAX_CRITICAL_SECTIONS);

	mutex[index].unlock();
}

/*
======================================================
wait and trigger events
we use a single lock to manipulate the conditions, CRITICAL_SECTION_SYS

the semantics match the win32 version. signals raised while no one is waiting stay raised until a wait happens (which then does a simple pass-through)

NOTE: we use the same mutex for all the events. I don't think this would become much of a problem
cond_wait unlocks atomically with setting the wait condition, and locks it back before exiting the function
the potential for time wasting lock waits is very low
======================================================
*/

/*
==================
Sys_WaitForEvent
==================
*/
void Sys_WaitForEvent(int index) {
	assert(index >= 0 && index < MAX_TRIGGER_EVENTS);

	Sys_EnterCriticalSection(CRITICAL_SECTION_SYS);

	assert(!waiting[index]);	// WaitForEvent from multiple threads? that wouldn't be good
	if (signaled[index]) {
		// emulate windows behaviour: signal has been raised already. clear and keep going
		signaled[index] = false;
	} else {
		waiting[index] = true;

		// The critical section is entered and left by hand, above and below,
		// so the lock is adopted rather than taken and released rather than
		// unlocked: wait() needs to own the mutex to release it atomically,
		// and it has to be still held when Sys_LeaveCriticalSection runs.
		//
		// This is the one place the recursion above must not have happened:
		// wait() unlocks once, so entering CRITICAL_SECTION_SYS twice and then
		// waiting would release neither. Nothing does - Sys_WaitForEvent takes
		// the section itself - and the same was true of the SDL version, whose
		// mutex was recursive for exactly the same reason.
		{
			std::unique_lock<std::recursive_mutex> lock( mutex[CRITICAL_SECTION_SYS], std::adopt_lock );
			cond[index].wait( lock );
			lock.release();
		}

		waiting[index] = false;
	}

	Sys_LeaveCriticalSection(CRITICAL_SECTION_SYS);
}

/*
==================
Sys_TriggerEvent
==================
*/
void Sys_TriggerEvent(int index) {
	assert(index >= 0 && index < MAX_TRIGGER_EVENTS);

	Sys_EnterCriticalSection(CRITICAL_SECTION_SYS);

	if (waiting[index]) {
		cond[index].notify_one();
	} else {
		// emulate windows behaviour: if no thread is waiting, leave the signal on so next wait keeps going
		signaled[index] = true;
	}

	Sys_LeaveCriticalSection(CRITICAL_SECTION_SYS);
}

/*
==================
Sys_CreateThread
==================
*/
void Sys_CreateThread(xthread_t function, void *parms, xthreadInfo& info, const char *name) {
	Sys_EnterCriticalSection();

	sysThread_t *handle = new sysThread_t;

	// The engine's thread functions return an int that nothing ever reads -
	// it was SDL's signature, not a result. Discarded here rather than plumbed
	// somewhere it would go on being ignored.
	handle->thread = std::thread( [function, parms]() { function( parms ); } );
	handle->id = handle->thread.get_id();

	info.name = name;
	info.threadHandle = handle;
	info.threadId = numericThreadID( handle->id );

	if (thread_count < MAX_THREADS)
		thread[thread_count++] = &info;
	else
		common->DPrintf("WARNING: MAX_THREADS reached\n");

	Sys_LeaveCriticalSection();
}

/*
==================
Sys_DestroyThread
==================
*/
void Sys_DestroyThread(xthreadInfo& info) {
	assert(info.threadHandle);

	info.threadHandle->thread.join();
	delete info.threadHandle;

	info.name = NULL;
	info.threadHandle = NULL;
	info.threadId = 0;

	Sys_EnterCriticalSection();

	for (size_t i = 0; i < thread_count; i++) {
		if (&info == thread[i]) {
			thread[i] = NULL;

			size_t j;
			for (j = i + 1; j < thread_count; j++)
				thread[j - 1] = thread[j];

			thread[j - 1] = NULL;
			thread_count--;

			break;
		}
	}

	Sys_LeaveCriticalSection( );
}

/*
==================
Sys_GetThreadName
find the name of the calling thread
==================
*/
const char *Sys_GetThreadName(int *index) {
	const char *name;

	Sys_EnterCriticalSection();

	std::thread::id id = std::this_thread::get_id();

	for (size_t i = 0; i < thread_count; i++) {
		if (thread[i] && thread[i]->threadHandle && id == thread[i]->threadHandle->id) {
			if (index)
				*index = (int)i;

			name = thread[i]->name;

			Sys_LeaveCriticalSection();

			return name;
		}
	}

	if (index)
		*index = -1;

	Sys_LeaveCriticalSection();

	return "main";
}


/*
==================
Sys_IsMainThread
returns true if the current thread is the main thread
==================
*/
bool Sys_IsMainThread() {
	if ( mainThreadIDset )
		return std::this_thread::get_id() == mainThreadID;
	// if this is called before mainThreadID is set, we haven't created
	// any threads yet so it should be the main thread
	return true;
}
