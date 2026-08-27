# dhewm3 - a Doom 3 source port.
#
# This fork supports macOS (as a universal binary) and Windows.
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

add_subdirectory(neo)
