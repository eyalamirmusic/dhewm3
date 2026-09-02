# dhewm3 - a Doom 3 source port, on the eacp platform layer.
#
# This fork supports macOS. The Windows host is named in plan.md and not
# written yet.
# The actual buildsystem lives in neo/, this file just wires it up so that
# `cmake -B build` works from the repository root.

cmake_minimum_required(VERSION 3.21)

project(dhewm3
	VERSION 1.5.5
	DESCRIPTION "A source port of Doom 3"
	HOMEPAGE_URL "https://dhewm3.org/"
	LANGUAGES C CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

include(Eacp)

add_subdirectory(neo)
