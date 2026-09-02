# eacp - the platform layer this port runs on. See ../plan.md, Phase 2.
#
# Fetched at the repository root rather than from inside neo/, for one reason:
# neo/ pins CMAKE_CXX_STANDARD to 11 for the Doom 3 code, and eacp is C++20.
# eacp sets its own standard in its own directory scope, so the two never have
# to agree - but only from a scope that has not already been set to 11.

include(CPM)

# eacp's main, because for as long as this port is under way the two repositories
# move together: a gap found here is fixed there (plan.md §5) and wanted here the
# same afternoon. A SHA would mean a bump commit per finding and a tree that is
# stale between them.
set(EACP_GIT_TAG "main" CACHE STRING
	"The eacp branch or commit to build against. A SHA pins; a branch tracks.")

# **A branch name on its own does not track that branch, and this file learned it
# the hard way.** CPM skips the download outright when the source directory
# already exists (CPM.cmake, `if(EXISTS ${download_directory})` -> CPM_SKIP_FETCH),
# whether that directory is a shared source cache or this build's own _deps. So a
# branch is cloned by the first configure and never looked at again. The first
# configure of this file asked for `main` and got eacp as it stood four months
# earlier - with none of the Phase 0 stencil work this port is built on - and
# reported it as a successful fetch of main.
#
# It was pinned to a SHA after that, which was honest but wrong for a port whose
# two halves are being written together. So the branch is back and this is what
# makes it true: fast-forward the clone to the remote before CPM looks at it. The
# reset is safe because this directory is CPM's to own - it is a throwaway clone
# under the build tree - and it is deliberately NOT done to a checkout somebody
# pointed CPM_eacp_SOURCE at, which is a working repository with a working
# repository's uncommitted changes in it.
function(eacp_track_branch tag)
	if(CPM_eacp_SOURCE)
		message(STATUS "eacp: using the local checkout at ${CPM_eacp_SOURCE}")
		return()
	endif()

	# A SHA is a pin and needs no refreshing - CPM's own clone is already at it.
	if(tag MATCHES "^[a-fA-F0-9]+$" AND NOT tag MATCHES "^[0-9]+$")
		return()
	endif()

	set(clone "${CMAKE_BINARY_DIR}/_deps/eacp-src")

	if(NOT IS_DIRECTORY "${clone}/.git")
		return()	# nothing fetched yet; CPM is about to clone it fresh
	endif()

	find_package(Git QUIET)

	if(NOT GIT_FOUND)
		message(WARNING "eacp: no git found, so '${tag}' is whatever "
			"${clone} already holds")
		return()
	endif()

	execute_process(COMMAND "${GIT_EXECUTABLE}" fetch --quiet origin "${tag}"
		WORKING_DIRECTORY "${clone}" RESULT_VARIABLE fetched)

	if(NOT fetched EQUAL 0)
		# Offline, most likely. Building against yesterday's eacp is better than
		# not building, but say so rather than letting it pass for current.
		message(WARNING "eacp: could not fetch '${tag}' - building against "
			"whatever ${clone} already holds")
		return()
	endif()

	execute_process(COMMAND "${GIT_EXECUTABLE}" reset --quiet --hard FETCH_HEAD
		WORKING_DIRECTORY "${clone}" RESULT_VARIABLE reset_result)

	if(NOT reset_result EQUAL 0)
		message(WARNING "eacp: could not check out '${tag}' in ${clone}")
		return()
	endif()

	execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
		WORKING_DIRECTORY "${clone}" OUTPUT_VARIABLE sha
		OUTPUT_STRIP_TRAILING_WHITESPACE)

	message(STATUS "eacp: tracking '${tag}', now at ${sha}")
endfunction()

# Re-run the regression gate after an eacp that moved under you, the same as
# after any other change to what the renderer is built on. Point
# CPM_eacp_SOURCE at a local checkout to develop against one, or set
# EACP_GIT_TAG to a SHA to pin.
CPMAddPackage(
	NAME eacp
	GITHUB_REPOSITORY eyalamirmusic/eacp
	GIT_TAG develop
	OPTIONS
		# The engine has no use for an embedded browser, and it is the most
		# expensive module eacp builds.
		"EACP_BUILD_WEBVIEW OFF"
)

# eacp only runs eacp_default_setup() when it is the top-level project, so as a
# CPM dependency it never sets the bundle plist template that its own
# set_default_target_setting() stamps onto app targets - which would leave the
# app on CMake's default Info.plist. PureDOOM's gap #3; same workaround.
if(APPLE AND NOT IOS)
	set(EACP_MACOS_PLIST "${eacp_SOURCE_DIR}/CMake/macOSBundleInfo.plist.in"
		CACHE INTERNAL "eacp macOS bundle Info.plist template")
endif()
