#include "App.h"

#include "Platform.h"

#include "sys/platform.h"
#include "sys/posix/posix_public.h"

int main(int argc, char** argv)
{
	// Before anything reads a path: Posix_InitSignalHandlers opens its crash log
	// next to the executable, and the file system is built on PATH_BASE.
	Sys_InitPaths();
	Posix_InitSignalHandlers();

	// argv is snapshotted into the app environment here and read back in
	// View::startEngine - common->Init runs from the view, once there is a
	// window for the renderer to eventually come up in.
	return eacp::Apps::run<dhewm3::App>(argc, argv);
}
