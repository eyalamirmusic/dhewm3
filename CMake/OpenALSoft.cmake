# OpenAL Soft - the only sound backend this port has.
#
# The two hosts get it from different places, and neither of them is "the system
# OpenAL": macOS ships one, but it is deprecated and puts its headers in OpenAL/
# rather than the AL/ that dhewm3 (and every other openal-soft user) includes,
# and Windows ships none at all.
#
# So: macOS asks Homebrew where the keg-only openal-soft lives and finds its
# CMake config package there, and Windows - which has no package manager the
# buildsystem can lean on - fetches and builds it like eacp. Either way what
# comes out is the OpenAL::OpenAL target the sound code links.

if(TARGET OpenAL::OpenAL)
	return()
endif()

if(APPLE)
	# Homebrew keeps openal-soft keg-only (macOS ships its own, deprecated, OpenAL),
	# so it isn't in CMake's default search path - add it if it's installed.
	find_program(BREW_EXECUTABLE brew)
	if(BREW_EXECUTABLE)
		execute_process(COMMAND "${BREW_EXECUTABLE}" --prefix openal-soft
		                OUTPUT_VARIABLE brew_openal_prefix
		                OUTPUT_STRIP_TRAILING_WHITESPACE
		                ERROR_QUIET)
		if(brew_openal_prefix)
			list(APPEND CMAKE_PREFIX_PATH "${brew_openal_prefix}")
		endif()
	endif()

	# Note: this deliberately requires openal-soft's CMake config package, for the
	# header-directory reason above.
	find_package(OpenAL REQUIRED CONFIG)
	return()
endif()

# An installed one still wins on Windows, for anyone who has one: a build against
# vcpkg's or a hand-built SDK should not silently fetch a second copy.
find_package(OpenAL QUIET CONFIG)
if(OpenAL_FOUND)
	message(STATUS "OpenAL Soft found at ${OpenAL_DIR}")
	return()
endif()

include(CPM)

set(OPENAL_GIT_TAG "1.24.3" CACHE STRING
	"The openal-soft tag to build against when there is no installed one.")

message(STATUS "No installed OpenAL Soft; fetching ${OPENAL_GIT_TAG}")

# Everything but the library itself is off: the router, the examples and the
# utilities are all things a game linking OpenAL has no use for, and each of them
# is a target that has to compile before dhewm3 can link.
CPMAddPackage(
	NAME openal-soft
	GITHUB_REPOSITORY kcat/openal-soft
	GIT_TAG ${OPENAL_GIT_TAG}
	OPTIONS
		"ALSOFT_EXAMPLES OFF"
		"ALSOFT_UTILS OFF"
		"ALSOFT_TESTS OFF"
		"ALSOFT_INSTALL OFF"
		"ALSOFT_INSTALL_CONFIG OFF"
		"ALSOFT_INSTALL_HRTF_DATA OFF"
		"ALSOFT_INSTALL_AMBDEC_PRESETS OFF"
		"ALSOFT_INSTALL_EXAMPLES OFF"
		"ALSOFT_INSTALL_UTILS OFF"
		"LIBTYPE SHARED"
)

# openal-soft's own target is plain `OpenAL`; the namespaced name is what its
# installed config package exports, and older tags do not alias it in-tree.
if(NOT TARGET OpenAL::OpenAL)
	add_library(OpenAL::OpenAL ALIAS OpenAL)
endif()
