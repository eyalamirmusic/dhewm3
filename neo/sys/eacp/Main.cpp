#include "App.h"

#include "Platform.h"

#include "sys/platform.h"

#ifndef _WIN32
	#include "sys/posix/posix_public.h"
#endif

int main(int argc, char** argv)
{
	// Before anything reads a path or prints a line. The two hosts spell this
	// differently and Platform.h says why: on macOS the paths have to be
	// resolved first, because Posix_InitSignalHandlers opens its crash log next
	// to the executable and the file system is built on PATH_BASE; on Windows
	// nothing has to be resolved and what has to be opened is the log.
#ifdef _WIN32
	Win_EarlyInit();
#else
	Sys_InitPaths();
	Posix_InitSignalHandlers();
#endif

	// argv is snapshotted into the app environment here and read back in
	// View::startEngine - common->Init runs from the view, once there is a
	// window for the renderer to eventually come up in.
	return eacp::Apps::run<dhewm3::App>(argc, argv);
}
