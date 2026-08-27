#include "App.h"

int main(int, char**)
{
	// argv is dropped rather than forwarded: the engine has not been started
	// yet, and Sys_ParseCommandLine is what will want it. Phase 2, step 2.
	return eacp::Apps::run<dhewm3::App>();
}
