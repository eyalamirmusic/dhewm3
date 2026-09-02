# eacp - already here, which is the whole of what this module says.
#
# imgui-eacp's root does find_package(EACP REQUIRED), and its own module answers
# it with CPMAddPackage(NAME EACP ...) from GitHub main. That would be a *second*
# eacp: CPM keys packages by name and `EACP` is not `eacp`, so nothing would
# notice the first one - and the second add_subdirectory of the same source
# fails on duplicate targets before it could.
#
# CMake/Eacp.cmake has already added it by the time imgui-eacp is, so there is
# nothing to fetch. This module wins over imgui-eacp's because the root
# CMakeLists appends this directory to CMAKE_MODULE_PATH first and find_package
# searches that list in order.

if(NOT TARGET eacp-sprites)
	message(FATAL_ERROR
		"FindEACP: eacp has not been added yet - include(Eacp) has to run "
		"before anything that asks for it")
endif()

set(EACP_FOUND TRUE)
