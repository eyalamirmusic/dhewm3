# eacp - the platform layer this port is moving onto. See ../plan.md, Phase 2.
#
# Fetched at the repository root rather than from inside neo/, for one reason:
# neo/ pins CMAKE_CXX_STANDARD to 11 for the Doom 3 code, and eacp is C++20.
# eacp sets its own standard in its own directory scope, so the two never have
# to agree - but only from a scope that has not already been set to 11.

if(NOT EACP)
	return()
endif()

include(CPM)

# A pinned commit, not `main`, and the reason is worth keeping: CPM keys its
# source cache on the *declaration*, so a branch name is fetched once and never
# again. The first configure of this file asked for `main` and got eacp as it
# stood on 2026-04-30 - four months stale, with none of the Phase 0 stencil work
# this port is built on - and reported it as a successful fetch of main. A branch
# tag under a source cache is therefore not "we track main"; it is "we track
# whatever main was the first time this machine built it", which is neither
# current nor reproducible. A SHA is at least honest, and bumping it is a commit
# somebody can see.
#
# Bump it deliberately, and re-run the regression gate when you do. Point
# CPM_eacp_SOURCE at a local checkout to develop against one.
CPMAddPackage(
	NAME eacp
	GITHUB_REPOSITORY eyalamirmusic/eacp
	GIT_TAG 05ffb0ca43f6cc4a06c98933cf0d912b6103b050
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
