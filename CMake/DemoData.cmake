# Downloads the free Doom 3 demo and unpacks its game data next to the dhewm3
# executable, so that a fresh build is runnable without owning the retail game.
#
# The demo ships as a makeself self-extracting archive containing x86 Linux
# binaries we have no use for. We never execute it: `--tar` makes it pipe its
# payload through `tar "$arg1" - $*` (line 200 of the .run), so this only ever
# reads the archive and pulls out the one file we want.
#
# The retail game data always wins over this - the engine only falls back to
# demo/ when base/ has no default.cfg (see idFileSystemLocal::Startup()).

if(NOT FETCH_DEMO_DATA OR NOT CORE)
	return()
endif()

# makeself needs a POSIX shell. Always there on macOS; on Windows it comes with
# Git for Windows, so warn and skip rather than breaking the build outright.
find_program(SH_PROGRAM sh)
if(NOT SH_PROGRAM)
	message(WARNING
		"FETCH_DEMO_DATA is ON, but no `sh` was found to unpack the demo installer "
		"(it ships with Git for Windows). Skipping the demo data.")
	return()
endif()

include(CPM)

if(NOT DEFINED ENV{CPM_SOURCE_CACHE})
	message(STATUS
		"FETCH_DEMO_DATA is ON: downloading the Doom 3 demo (~485 MB) into the build tree. "
		"Set the CPM_SOURCE_CACHE environment variable to keep it across clean builds, "
		"or configure with -DFETCH_DEMO_DATA=OFF to skip it.")
endif()

CPMAddPackage(
	NAME doom3demo
	VERSION 1.1.1286
	URL "https://files.holarse-linuxgaming.de/native/Spiele/Doom%203/Demo/doom3-linux-1.1.1286-demo.x86.run"
	URL_HASH SHA256=b42260fd08feb13c2f035a3746f8c1e3472151f0f781b8a2d1da231dae818083
	DOWNLOAD_ONLY YES
	# It's a shell script with an appended payload, not an archive CMake can unpack.
	DOWNLOAD_NO_EXTRACT TRUE
)

set(demo_installer "${doom3demo_SOURCE_DIR}/doom3-linux-1.1.1286-demo.x86.run")

# Where fs_basepath will point at runtime. On Windows that's the directory
# holding dhewm3.exe; on macOS Sys_GetPath(PATH_BASE) takes the *parent* of the
# .app bundle (sys/eacp/Platform.mm), which is the same directory. Both are
# the target's runtime output dir, which multi-config generators suffix with the
# configuration name.
get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(is_multi_config)
	set(demo_basepath "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
else()
	set(demo_basepath "${CMAKE_CURRENT_BINARY_DIR}")
endif()

# tar recreates the demo/ directory itself, so extracting from basepath lands the
# data in <basepath>/demo/demo00.pk4 - exactly the game directory the engine
# falls back to. Naming the .pk4 as OUTPUT keeps the 462 MB unpack from repeating
# on every build.
add_custom_command(
	OUTPUT "${demo_basepath}/demo/demo00.pk4"
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${demo_basepath}"
	COMMAND "${CMAKE_COMMAND}" -E chdir "${demo_basepath}"
	        "${SH_PROGRAM}" "${demo_installer}" --tar xf demo/
	DEPENDS "${demo_installer}"
	COMMENT "Unpacking demo00.pk4 (462 MB) into the demo/ game directory"
	VERBATIM
)

add_custom_target(demo-data DEPENDS "${demo_basepath}/demo/demo00.pk4")

# Hang it off the executable rather than relying on ALL: building just that
# target should still leave you with something you can run.
add_dependencies(dhewm3 demo-data)
