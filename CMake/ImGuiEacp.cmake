# imgui-eacp - Dear ImGui's two backends for eacp, which is what lights the F10
# settings menu back up. See ../plan.md, Phase 2 step 7.
#
# The port uses the renderer half (Gui::DrawRenderer) and the input mapping
# (Gui::KeyMap) and not Gui::ImGuiView: that class is a GPUView with its own
# context, its own pass and its own frame timing - a panel - and the settings
# menu is an overlay inside the engine's frame, drawn into the render target
# 4e.1 composes into.
#
# Added at the repository root and after Eacp.cmake, for two reasons. neo/ pins
# CMAKE_CXX_STANDARD to 11 for the Doom 3 code and this is C++20; and
# imgui-eacp's own find_package(EACP) / find_package(ImGui) have to find this
# directory's modules rather than its own, which they do because the root
# CMakeLists appended this directory to CMAKE_MODULE_PATH before imgui-eacp
# appends its own and find_package searches that list in order.

include(CPM)

# Ahead of everything, including the CPM_modules directory CPM generates a
# Findeacp.cmake into. That file answers find_package(EACP) too on a
# case-insensitive filesystem and answers it correctly - but CMake warns about
# the case mismatch every configure, and it would not be found at all on a
# case-sensitive one. FindEACP.cmake here is the answer on both.
list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_CURRENT_LIST_DIR}")

# The same convention as EACP_GIT_TAG, and for the same reason: while this port
# is under way the two repositories move together, so a branch that tracks beats
# a SHA that has to be bumped. Point CPM_imgui-eacp_SOURCE at a local checkout to
# develop against one.
set(IMGUI_EACP_GIT_TAG "main" CACHE STRING
	"The imgui-eacp branch or commit to build against. A SHA pins; a branch tracks.")

# Eacp.cmake's finding, applied again: CPM skips the download outright when the
# source directory already exists, so a branch is cloned once and never looked at
# again. Fast-forward the clone before CPM sees it.
function(imgui_eacp_track_branch tag)
	if(CPM_imgui-eacp_SOURCE)
		message(STATUS "imgui-eacp: using the local checkout at ${CPM_imgui-eacp_SOURCE}")
		return()
	endif()

	if(tag MATCHES "^[a-fA-F0-9]+$" AND NOT tag MATCHES "^[0-9]+$")
		return()
	endif()

	set(clone "${CMAKE_BINARY_DIR}/_deps/imgui-eacp-src")

	if(NOT IS_DIRECTORY "${clone}/.git")
		return()
	endif()

	find_package(Git QUIET)

	if(NOT GIT_FOUND)
		message(WARNING "imgui-eacp: no git found, so '${tag}' is whatever "
			"${clone} already holds")
		return()
	endif()

	execute_process(COMMAND "${GIT_EXECUTABLE}" fetch --quiet origin "${tag}"
		WORKING_DIRECTORY "${clone}" RESULT_VARIABLE fetched)

	if(NOT fetched EQUAL 0)
		message(WARNING "imgui-eacp: could not fetch '${tag}' - building against "
			"whatever ${clone} already holds")
		return()
	endif()

	execute_process(COMMAND "${GIT_EXECUTABLE}" reset --quiet --hard FETCH_HEAD
		WORKING_DIRECTORY "${clone}" RESULT_VARIABLE reset_result)

	if(NOT reset_result EQUAL 0)
		message(WARNING "imgui-eacp: could not check out '${tag}' in ${clone}")
		return()
	endif()

	execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
		WORKING_DIRECTORY "${clone}" OUTPUT_VARIABLE sha
		OUTPUT_STRIP_TRAILING_WHITESPACE)

	message(STATUS "imgui-eacp: tracking '${tag}', now at ${sha}")
endfunction()

imgui_eacp_track_branch("${IMGUI_EACP_GIT_TAG}")

# Its apps and its tests turn themselves off when it is not the top-level
# project, so this brings in one static library and nothing else.
CPMAddPackage(
	NAME imgui-eacp
	GITHUB_REPOSITORY eyalamirmusic/imgui-eacp
	GIT_TAG ${IMGUI_EACP_GIT_TAG}
)
