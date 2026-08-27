# dhewm3 - a Doom 3 source port.
#
# Supported targets: macOS (universal binary) and Windows.
# Everything is driven from the repository root, see ../CMakeLists.txt.

# dhewm3 used to be configured by pointing CMake at neo/ directly. That still half-works
# (CMake makes up a project for you), but it skips ../CMakeLists.txt and with it the
# universal-binary setup, so it would quietly produce a single-architecture build.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
	message(FATAL_ERROR "Configure from the repository root, not from neo/, e.g. `cmake -B build`")
endif()

if(NOT APPLE AND NOT WIN32)
	message(FATAL_ERROR "This fork of dhewm3 only supports macOS and Windows.")
endif()

if(APPLE)
	# sys/osx/ contains Objective-C(++) sources
	enable_language(OBJC)
	enable_language(OBJCXX)
endif()

include(CheckCXXCompilerFlag)

# --------------------------------------------------------------------------- #
# Options
# --------------------------------------------------------------------------- #

option(CORE				"Build the dhewm3 executable" ON)
option(BASE				"Build the base game code (Doom 3)" ON)
option(D3XP				"Build the d3xp game code (Resurrection of Evil)" ON)
option(HARDLINK_GAME	"Compile the game code into the executable instead of separate game libraries" OFF)
option(REPRODUCIBLE_BUILD "Replace __DATE__ and __TIME__ with hardcoded values for reproducible builds" OFF)

if(NOT MSVC)
	option(ASAN			"Enable the Address Sanitizer" OFF)
	option(UBSAN		"Enable the Undefined Behavior Sanitizer (implies HARDLINK_GAME)" OFF)

	if(UBSAN AND NOT HARDLINK_GAME)
		message(STATUS "UBSAN requires linking the gamecode into the executable, enabling HARDLINK_GAME")
		set(HARDLINK_GAME ON)
	endif()
endif()

get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT is_multi_config AND NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Dear ImGui (used for the dhewm3 settings menu) needs C++11.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --------------------------------------------------------------------------- #
# Dependencies
# --------------------------------------------------------------------------- #

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
endif()

find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3-shared)

# Note: this deliberately requires openal-soft's CMake config package. macOS also
# ships an OpenAL.framework, but it's deprecated and puts its headers in OpenAL/
# instead of the AL/ that dhewm3 (and every other openal-soft user) includes.
find_package(OpenAL REQUIRED CONFIG)

find_package(CURL)
if(CURL_FOUND)
	message(STATUS "libcurl found, server downloads enabled")
else()
	message(STATUS "libcurl not found, server downloads won't be available (dhewm3 works fine without it)")
endif()

# A universal binary needs universal dependencies, and neither Homebrew nor vcpkg
# ships those - catch that at configure time instead of at link time.
function(check_universal_dependency target)
	list(LENGTH CMAKE_OSX_ARCHITECTURES num_archs)
	if(num_archs LESS 2 OR NOT TARGET ${target})
		return()
	endif()
	get_target_property(lib ${target} IMPORTED_LOCATION)
	if(NOT lib)
		get_target_property(configs ${target} IMPORTED_CONFIGURATIONS)
		list(GET configs 0 config)
		get_target_property(lib ${target} IMPORTED_LOCATION_${config})
	endif()
	if(NOT lib OR NOT EXISTS "${lib}")
		return()
	endif()
	execute_process(COMMAND lipo -archs "${lib}" OUTPUT_VARIABLE have OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
	separate_arguments(have)
	foreach(arch IN LISTS CMAKE_OSX_ARCHITECTURES)
		if(NOT arch IN_LIST have)
			message(FATAL_ERROR
				"${lib}\n"
				"  is ${have} only, but this is a universal build (${CMAKE_OSX_ARCHITECTURES}).\n"
				"  Either point CMAKE_PREFIX_PATH at universal builds of SDL3 and openal-soft,\n"
				"  or configure a native-only build with -DCMAKE_OSX_ARCHITECTURES=${CMAKE_SYSTEM_PROCESSOR}")
		endif()
	endforeach()
endfunction()

if(APPLE)
	check_universal_dependency(SDL3::SDL3-shared)
	check_universal_dependency(OpenAL::OpenAL)
endif()

# --------------------------------------------------------------------------- #
# Compiler and linker flags
# --------------------------------------------------------------------------- #

# Adds -Wno-<name>, but only if the compiler actually knows that warning.
# (Testing the negative form is useless - clang accepts unknown -Wno-* silently.)
function(disable_warning name)
	string(MAKE_C_IDENTIFIER "have_warning_${name}" have_it)
	check_cxx_compiler_flag("-W${name}" ${have_it})
	if(${have_it})
		add_compile_options("-Wno-${name}")
	endif()
endfunction()

if(MSVC)
	add_compile_options(/MP)			# build with all cores
	add_compile_options(/W3)
	add_compile_options(/we4840)		# passing a class to a vararg function (probably printf-like)
	# truncating int<->pointer conversions are errors, for 64bit safety
	add_compile_options(/we4306 /we4311 /we4312 /we4302)
	add_compile_options(
		/wd4100		# unreferenced formal parameter
		/wd4127		# conditional expression is constant
		/wd4244		# possible loss of data
		/wd4245		# signed/unsigned mismatch
		/wd4267		# possible loss of data
		/wd4714		# 'function' marked as __forceinline not inlined
		/wd4996		# 'function': was declared deprecated
		/wd4068		# unknown pragma
		/wd4458		# declaration of 'variable' hides class member
	)
	# TypeInfo.cpp does "#define private public"
	add_compile_definitions(_ALLOW_KEYWORD_MACROS)

	set(CMAKE_C_FLAGS_DEBUG				"-D_DEBUG /Od /Zi /MDd")
	set(CMAKE_C_FLAGS_RELEASE			"/Ox /Oy /MD")
	set(CMAKE_C_FLAGS_RELWITHDEBINFO	"/Ox /Oy /Zi /MD")
	set(CMAKE_C_FLAGS_MINSIZEREL		"/Ox /Oy /Os /MD")

	# 8MB stack instead of 1MB, so big models don't overflow it with _alloca()
	add_link_options(/STACK:8388608)
else() # GCC / clang (incl. AppleClang and MinGW)
	add_compile_options(-pipe -Wall)

	# Doom 3's code does a lot of type punning and predates strict aliasing rules
	add_compile_options(-fno-strict-aliasing)

	# let the compiler emit CPU instructions for math functions instead of libm calls
	add_compile_options(-fno-math-errno -fno-trapping-math -ffinite-math-only)

	# don't let the compiler contract a*b+c into fma(), it changes results
	# (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=100839)
	check_cxx_compiler_flag("-ffp-contract=off" cxx_has_fp_contract)
	if(cxx_has_fp_contract)
		add_compile_options(-ffp-contract=off)
	endif()

	if(NOT WIN32)
		add_compile_options(-fvisibility=hidden)
	endif()

	add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>)

	# Warnings that fire hundreds or thousands of times in this 2004 codebase and
	# would otherwise drown out anything worth reading:
	disable_warning(sign-compare)
	disable_warning(switch)
	disable_warning(c++20-compat)		# variables named "requires"
	disable_warning(class-memaccess)	# memcpy() on non-trivial classes (GCC's name)
	disable_warning(nontrivial-memcall)	# ... and clang's name for it
	disable_warning(deprecated-declarations)	# sprintf() etc, deprecated by recent macOS SDKs
	disable_warning(unused-but-set-variable)
	disable_warning(unused-private-field)
	disable_warning(unused-const-variable)
	disable_warning(unused-function)
	disable_warning(self-assign-field)

	set(CMAKE_C_FLAGS_DEBUG				"-g -ggdb -D_DEBUG -O0")
	set(CMAKE_C_FLAGS_RELEASE			"-O2 -fomit-frame-pointer")
	set(CMAKE_C_FLAGS_RELWITHDEBINFO	"-g -ggdb -O2 -fno-omit-frame-pointer")
	set(CMAKE_C_FLAGS_MINSIZEREL		"-Os -fomit-frame-pointer")

	if(ASAN)
		add_compile_options(-fsanitize=address)
		add_link_options(-fsanitize=address)
	endif()
	if(UBSAN)
		add_compile_options(-fsanitize=undefined)
		add_link_options(-fsanitize=undefined)
	endif()

	if(WIN32) # MinGW
		# 8MB stack instead of 1MB, so big models don't overflow it with _alloca()
		add_link_options(-Wl,--stack,8388608)
	endif()
endif()

# Disable assertions in release builds. RelWithDebInfo deliberately keeps them,
# which is why these flags are spelled out instead of using CMake's defaults.
set(CMAKE_C_FLAGS_RELEASE			"-DNDEBUG ${CMAKE_C_FLAGS_RELEASE}")
set(CMAKE_C_FLAGS_MINSIZEREL		"-DNDEBUG ${CMAKE_C_FLAGS_MINSIZEREL}")

foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
	set(CMAKE_CXX_FLAGS_${cfg} "${CMAKE_C_FLAGS_${cfg}}")
	if(APPLE)
		# keep the Objective-C(++) sources in sys/osx/ on the same flags (esp. NDEBUG)
		set(CMAKE_OBJC_FLAGS_${cfg}   "${CMAKE_C_FLAGS_${cfg}}")
		set(CMAKE_OBJCXX_FLAGS_${cfg} "${CMAKE_C_FLAGS_${cfg}}")
	endif()
endforeach()

# --------------------------------------------------------------------------- #
# Global definitions
# --------------------------------------------------------------------------- #

if(APPLE)
	set(os "macosx")
	add_compile_definitions(MACOS_X=1)
else()
	set(os "windows")
	add_compile_definitions(
		WINVER=0x0501
		_WIN32_WINNT=0x0501
	)
endif()

# D3_OSTYPE ends up in savegames, D3_ARCH and D3_SIZEOFPTR are set in sys/platform.h
# from the compiler's own predefines (they differ per slice of a universal binary,
# so the buildsystem can't know them).
add_compile_definitions(D3_OSTYPE="${os}")
# Both supported platforms are little endian on every architecture they run on.
add_compile_definitions(D3_IS_BIG_ENDIAN=0)
add_compile_definitions(D3_SDL3=1)

if(REPRODUCIBLE_BUILD)
	add_compile_definitions(ID_REPRODUCIBLE_BUILD)
endif()

if(CURL_FOUND)
	set(ID_ENABLE_CURL ON)
endif()

configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/config.h.in"
	"${CMAKE_CURRENT_BINARY_DIR}/config.h"
)

include_directories("${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")

# --------------------------------------------------------------------------- #
# Source lists
# --------------------------------------------------------------------------- #

# Optimize a source file even in debug builds, to keep loading times bearable.
# Used for the bundled libraries (stb_*, miniz) that we don't debug anyway.
function(always_optimize_sourcefile srcfilename)
	if(MSVC)
		set_source_files_properties(${srcfilename} PROPERTIES COMPILE_OPTIONS "/Ox")
	else()
		set_source_files_properties(${srcfilename} PROPERTIES COMPILE_OPTIONS "-O2")
	endif()
endfunction()

# Headers are globbed in and added to the targets purely so they show up in IDEs.
function(add_globbed_headers SRCLIST PATHPREFIX)
	file(GLOB_RECURSE tmp_hdrs RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${PATHPREFIX}/*.h")
	set(${SRCLIST} ${tmp_hdrs} ${${SRCLIST}} PARENT_SCOPE)
endfunction()

set(src_renderer
	renderer/Cinematic.cpp
	renderer/GuiModel.cpp
	renderer/Image_files.cpp
	renderer/Image_init.cpp
	renderer/Image_load.cpp
	renderer/Image_process.cpp
	renderer/Image_program.cpp
	renderer/Interaction.cpp
	renderer/Material.cpp
	renderer/MegaTexture.cpp
	renderer/Model.cpp
	renderer/ModelDecal.cpp
	renderer/ModelManager.cpp
	renderer/ModelOverlay.cpp
	renderer/Model_beam.cpp
	renderer/Model_ase.cpp
	renderer/Model_liquid.cpp
	renderer/Model_lwo.cpp
	renderer/Model_ma.cpp
	renderer/Model_md3.cpp
	renderer/Model_md5.cpp
	renderer/Model_prt.cpp
	renderer/Model_sprite.cpp
	renderer/RenderEntity.cpp
	renderer/RenderSystem.cpp
	renderer/RenderSystem_init.cpp
	renderer/RenderWorld.cpp
	renderer/RenderWorld_demo.cpp
	renderer/RenderWorld_load.cpp
	renderer/RenderWorld_portals.cpp
	renderer/VertexCache.cpp
	renderer/draw_arb2.cpp
	renderer/draw_common.cpp
	renderer/tr_backend.cpp
	renderer/tr_deform.cpp
	renderer/tr_font.cpp
	renderer/tr_guisurf.cpp
	renderer/tr_light.cpp
	renderer/tr_lightrun.cpp
	renderer/tr_main.cpp
	renderer/tr_orderIndexes.cpp
	renderer/tr_polytope.cpp
	renderer/tr_render.cpp
	renderer/tr_rendertools.cpp
	renderer/tr_shadowbounds.cpp
	renderer/tr_stencilshadow.cpp
	renderer/tr_subview.cpp
	renderer/tr_trace.cpp
	renderer/tr_trisurf.cpp
	renderer/tr_turboshadow.cpp

	renderer/stblib_impls.c
)

always_optimize_sourcefile(renderer/stblib_impls.c)

add_globbed_headers(src_renderer "renderer")

set(src_framework
	framework/CVarSystem.cpp
	framework/CmdSystem.cpp
	framework/Common.cpp
	framework/Compressor.cpp
	framework/Console.cpp
	framework/DemoFile.cpp
	framework/DeclAF.cpp
	framework/DeclEntityDef.cpp
	framework/DeclFX.cpp
	framework/DeclManager.cpp
	framework/DeclParticle.cpp
	framework/DeclPDA.cpp
	framework/DeclSkin.cpp
	framework/DeclTable.cpp
	framework/Dhewm3SettingsMenu.cpp
	framework/EditField.cpp
	framework/EventLoop.cpp
	framework/File.cpp
	framework/FileSystem.cpp
	framework/KeyInput.cpp
	framework/UsercmdGen.cpp
	framework/Session_menu.cpp
	framework/Session.cpp
	framework/async/AsyncClient.cpp
	framework/async/AsyncNetwork.cpp
	framework/async/AsyncServer.cpp
	framework/async/MsgChannel.cpp
	framework/async/NetworkSystem.cpp
	framework/async/ServerScan.cpp
	framework/miniz/miniz.c
	framework/minizip/ioapi.c
	framework/minizip/unzip.cpp
)

always_optimize_sourcefile(framework/miniz/miniz.c)
always_optimize_sourcefile(framework/minizip/ioapi.c)
always_optimize_sourcefile(framework/minizip/unzip.cpp)

add_globbed_headers(src_framework "framework")

set(src_cm
	cm/CollisionModel_contacts.cpp
	cm/CollisionModel_contents.cpp
	cm/CollisionModel_debug.cpp
	cm/CollisionModel_files.cpp
	cm/CollisionModel_load.cpp
	cm/CollisionModel_rotate.cpp
	cm/CollisionModel_trace.cpp
	cm/CollisionModel_translate.cpp
)

add_globbed_headers(src_cm "cm")

set(src_dmap
	tools/compilers/dmap/dmap.cpp
	tools/compilers/dmap/facebsp.cpp
	tools/compilers/dmap/gldraw.cpp
	tools/compilers/dmap/glfile.cpp
	tools/compilers/dmap/leakfile.cpp
	tools/compilers/dmap/map.cpp
	tools/compilers/dmap/optimize.cpp
	tools/compilers/dmap/output.cpp
	tools/compilers/dmap/portals.cpp
	tools/compilers/dmap/shadowopt3.cpp
	tools/compilers/dmap/tritjunction.cpp
	tools/compilers/dmap/tritools.cpp
	tools/compilers/dmap/ubrush.cpp
	tools/compilers/dmap/usurface.cpp
)

add_globbed_headers(src_dmap "tools/compilers/dmap")

set(src_aas
	tools/compilers/aas/AASBuild.cpp
	tools/compilers/aas/AASBuild_file.cpp
	tools/compilers/aas/AASBuild_gravity.cpp
	tools/compilers/aas/AASBuild_ledge.cpp
	tools/compilers/aas/AASBuild_merge.cpp
	tools/compilers/aas/AASCluster.cpp
	tools/compilers/aas/AASFile.cpp
	tools/compilers/aas/AASFile_optimize.cpp
	tools/compilers/aas/AASFile_sample.cpp
	tools/compilers/aas/AASReach.cpp
	tools/compilers/aas/AASFileManager.cpp
	tools/compilers/aas/Brush.cpp
	tools/compilers/aas/BrushBSP.cpp
)

add_globbed_headers(src_aas "tools/compilers/aas")

set(src_roq
	tools/compilers/roqvq/NSBitmapImageRep.cpp
	tools/compilers/roqvq/codec.cpp
	tools/compilers/roqvq/roq.cpp
	tools/compilers/roqvq/roqParam.cpp
)

add_globbed_headers(src_roq "tools/compilers/roqvq")

set(src_renderbump
	tools/compilers/renderbump/renderbump.cpp
)

add_globbed_headers(src_renderbump "tools/compilers/renderbump")

set(src_snd
	sound/snd_cache.cpp
	sound/snd_decoder.cpp
	sound/snd_efxfile.cpp
	sound/snd_emitter.cpp
	sound/snd_shader.cpp
	sound/snd_system.cpp
	sound/snd_wavefile.cpp
	sound/snd_world.cpp

	sound/stbvorbis_impl.c
)

always_optimize_sourcefile(sound/stbvorbis_impl.c)

add_globbed_headers(src_snd "sound")

set(src_ui
	ui/BindWindow.cpp
	ui/ChoiceWindow.cpp
	ui/DeviceContext.cpp
	ui/EditWindow.cpp
	ui/FieldWindow.cpp
	ui/GameBearShootWindow.cpp
	ui/GameBustOutWindow.cpp
	ui/GameSSDWindow.cpp
	ui/GuiScript.cpp
	ui/ListGUI.cpp
	ui/ListWindow.cpp
	ui/MarkerWindow.cpp
	ui/RegExp.cpp
	ui/RenderWindow.cpp
	ui/SimpleWindow.cpp
	ui/SliderWindow.cpp
	ui/UserInterface.cpp
	ui/Window.cpp
	ui/Winvar.cpp
)

add_globbed_headers(src_ui "ui")

set(src_idlib
	idlib/bv/Bounds.cpp
	idlib/bv/Frustum.cpp
	idlib/bv/Sphere.cpp
	idlib/bv/Box.cpp
	idlib/geometry/DrawVert.cpp
	idlib/geometry/Winding2D.cpp
	idlib/geometry/Surface_SweptSpline.cpp
	idlib/geometry/Winding.cpp
	idlib/geometry/Surface.cpp
	idlib/geometry/Surface_Patch.cpp
	idlib/geometry/TraceModel.cpp
	idlib/geometry/JointTransform.cpp
	idlib/hashing/CRC32.cpp
	idlib/hashing/MD4.cpp
	idlib/hashing/MD5.cpp
	idlib/math/Angles.cpp
	idlib/math/Lcp.cpp
	idlib/math/Math.cpp
	idlib/math/Matrix.cpp
	idlib/math/Ode.cpp
	idlib/math/Plane.cpp
	idlib/math/Pluecker.cpp
	idlib/math/Polynomial.cpp
	idlib/math/Quat.cpp
	idlib/math/Rotation.cpp
	idlib/math/Simd.cpp
	idlib/math/Simd_Generic.cpp
	idlib/math/Simd_AltiVec.cpp
	idlib/math/Simd_MMX.cpp
	idlib/math/Simd_3DNow.cpp
	idlib/math/Simd_SSE.cpp
	idlib/math/Simd_SSE2.cpp
	idlib/math/Simd_SSE3.cpp
	idlib/math/Vector.cpp
	idlib/BitMsg.cpp
	idlib/LangDict.cpp
	idlib/Lexer.cpp
	idlib/Lib.cpp
	idlib/containers/HashIndex.cpp
	idlib/Dict.cpp
	idlib/Str.cpp
	idlib/Parser.cpp
	idlib/MapFile.cpp
	idlib/CmdArgs.cpp
	idlib/Token.cpp
	idlib/Base64.cpp
	idlib/Timer.cpp
	idlib/Heap.cpp
)

add_globbed_headers(src_idlib "idlib")

set(src_imgui
	libs/imgui/backends/imgui_impl_sdl3.cpp
	libs/imgui/backends/imgui_impl_opengl2.cpp

	libs/imgui/imgui.h
	libs/imgui/imgui.cpp
	libs/imgui/imgui_draw.cpp
	libs/imgui/imgui_tables.cpp
	libs/imgui/imgui_widgets.cpp

	libs/imgui/imgui_demo.cpp

	sys/sys_imgui.h
	sys/sys_imgui.cpp
	sys/imgui_savestyle.cpp
)


set(src_game
	game/AF.cpp
	game/AFEntity.cpp
	game/Actor.cpp
	game/Camera.cpp
	game/Entity.cpp
	game/BrittleFracture.cpp
	game/Fx.cpp
	game/GameEdit.cpp
	game/Game_local.cpp
	game/Game_network.cpp
	game/Item.cpp
	game/IK.cpp
	game/Light.cpp
	game/Misc.cpp
	game/Mover.cpp
	game/Moveable.cpp
	game/MultiplayerGame.cpp
	game/Player.cpp
	game/PlayerIcon.cpp
	game/PlayerView.cpp
	game/Projectile.cpp
	game/Pvs.cpp
	game/SecurityCamera.cpp
	game/SmokeParticles.cpp
	game/Sound.cpp
	game/Target.cpp
	game/Trigger.cpp
	game/Weapon.cpp
	game/WorldSpawn.cpp
	game/ai/AAS.cpp
	game/ai/AAS_debug.cpp
	game/ai/AAS_pathing.cpp
	game/ai/AAS_routing.cpp
	game/ai/AI.cpp
	game/ai/AI_events.cpp
	game/ai/AI_pathing.cpp
	game/ai/AI_Vagary.cpp
	game/gamesys/DebugGraph.cpp
	game/gamesys/Class.cpp
	game/gamesys/Event.cpp
	game/gamesys/SaveGame.cpp
	game/gamesys/SysCmds.cpp
	game/gamesys/SysCvar.cpp
	game/gamesys/TypeInfo.cpp
	game/anim/Anim.cpp
	game/anim/Anim_Blend.cpp
	game/anim/Anim_Import.cpp
	game/anim/Anim_Testmodel.cpp
	game/script/Script_Compiler.cpp
	game/script/Script_Interpreter.cpp
	game/script/Script_Program.cpp
	game/script/Script_Thread.cpp
	game/physics/Clip.cpp
	game/physics/Force.cpp
	game/physics/Force_Constant.cpp
	game/physics/Force_Drag.cpp
	game/physics/Force_Field.cpp
	game/physics/Force_Spring.cpp
	game/physics/Physics.cpp
	game/physics/Physics_AF.cpp
	game/physics/Physics_Actor.cpp
	game/physics/Physics_Base.cpp
	game/physics/Physics_Monster.cpp
	game/physics/Physics_Parametric.cpp
	game/physics/Physics_Player.cpp
	game/physics/Physics_RigidBody.cpp
	game/physics/Physics_Static.cpp
	game/physics/Physics_StaticMulti.cpp
	game/physics/Push.cpp
)

add_globbed_headers(src_game "game")

set(src_d3xp
	d3xp/AF.cpp
	d3xp/AFEntity.cpp
	d3xp/Actor.cpp
	d3xp/Camera.cpp
	d3xp/Entity.cpp
	d3xp/BrittleFracture.cpp
	d3xp/Fx.cpp
	d3xp/GameEdit.cpp
	d3xp/Game_local.cpp
	d3xp/Game_network.cpp
	d3xp/Item.cpp
	d3xp/IK.cpp
	d3xp/Light.cpp
	d3xp/Misc.cpp
	d3xp/Mover.cpp
	d3xp/Moveable.cpp
	d3xp/MultiplayerGame.cpp
	d3xp/Player.cpp
	d3xp/PlayerIcon.cpp
	d3xp/PlayerView.cpp
	d3xp/Projectile.cpp
	d3xp/Pvs.cpp
	d3xp/SecurityCamera.cpp
	d3xp/SmokeParticles.cpp
	d3xp/Sound.cpp
	d3xp/Target.cpp
	d3xp/Trigger.cpp
	d3xp/Weapon.cpp
	d3xp/WorldSpawn.cpp
	d3xp/ai/AAS.cpp
	d3xp/ai/AAS_debug.cpp
	d3xp/ai/AAS_pathing.cpp
	d3xp/ai/AAS_routing.cpp
	d3xp/ai/AI.cpp
	d3xp/ai/AI_events.cpp
	d3xp/ai/AI_pathing.cpp
	d3xp/ai/AI_Vagary.cpp
	d3xp/gamesys/DebugGraph.cpp
	d3xp/gamesys/Class.cpp
	d3xp/gamesys/Event.cpp
	d3xp/gamesys/SaveGame.cpp
	d3xp/gamesys/SysCmds.cpp
	d3xp/gamesys/SysCvar.cpp
	d3xp/gamesys/TypeInfo.cpp
	d3xp/anim/Anim.cpp
	d3xp/anim/Anim_Blend.cpp
	d3xp/anim/Anim_Import.cpp
	d3xp/anim/Anim_Testmodel.cpp
	d3xp/script/Script_Compiler.cpp
	d3xp/script/Script_Interpreter.cpp
	d3xp/script/Script_Program.cpp
	d3xp/script/Script_Thread.cpp
	d3xp/physics/Clip.cpp
	d3xp/physics/Force.cpp
	d3xp/physics/Force_Constant.cpp
	d3xp/physics/Force_Drag.cpp
	d3xp/physics/Force_Field.cpp
	d3xp/physics/Force_Spring.cpp
	d3xp/physics/Physics.cpp
	d3xp/physics/Physics_AF.cpp
	d3xp/physics/Physics_Actor.cpp
	d3xp/physics/Physics_Base.cpp
	d3xp/physics/Physics_Monster.cpp
	d3xp/physics/Physics_Parametric.cpp
	d3xp/physics/Physics_Player.cpp
	d3xp/physics/Physics_RigidBody.cpp
	d3xp/physics/Physics_Static.cpp
	d3xp/physics/Physics_StaticMulti.cpp
	d3xp/physics/Push.cpp
	d3xp/Grabber.cpp
	d3xp/physics/Force_Grab.cpp
)

add_globbed_headers(src_d3xp "d3xp")

set(src_debuggerServer
	tools/debugger/DebuggerBreakpoint.h
	tools/debugger/DebuggerBreakpoint.cpp
	tools/debugger/DebuggerServer.h
	tools/debugger/DebuggerServer.cpp
	tools/debugger/DebuggerScript.h
	tools/debugger/DebuggerScript.cpp
	tools/debugger/DebuggerMessages.h
	tools/debugger/debugger.cpp

	tools/edit_stub.cpp
	tools/edit_public.h
	tools/debugger/debugger_common.h
)

set(src_core
	${src_renderer}
	${src_framework}
	${src_cm}
	${src_dmap}
	${src_aas}
	${src_roq}
	${src_renderbump}
	${src_snd}
	${src_ui}
	${src_debuggerServer}
	${src_imgui}
)

if(APPLE)
	set(osx_resources
		"sys/osx/Doom3.icns"
		"sys/osx/Doom 3.rsrc"
	)
	set_source_files_properties(${osx_resources} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

	set(src_sys
		sys/cpu.cpp
		sys/threads.cpp
		sys/events.cpp
		sys/sys_local.cpp
		sys/glimp.cpp
		sys/posix/posix_net.cpp
		sys/posix/posix_main.cpp
		sys/osx/DOOMController.mm
		sys/osx/macosx_misc.mm
		sys/osx/SDLMain.m
		${osx_resources}
	)
else() # Windows
	set(src_sys
		sys/cpu.cpp
		sys/threads.cpp
		sys/events.cpp
		sys/sys_local.cpp
		sys/glimp.cpp
		sys/win32/win_input.cpp
		sys/win32/win_main.cpp
		sys/win32/win_net.cpp
		sys/win32/win_shared.cpp
		sys/win32/win_syscon.cpp
		sys/win32/rc/dhewm3.rc

		sys/platform.h
		sys/sys_local.h
		sys/sys_public.h
		sys/win32/win_local.h
	)
endif()

# --------------------------------------------------------------------------- #
# Targets
# --------------------------------------------------------------------------- #

add_library(idlib STATIC ${src_idlib})
target_link_libraries(idlib PUBLIC SDL3::Headers) # for SDL_endian.h
set_target_properties(idlib PROPERTIES POSITION_INDEPENDENT_CODE ON)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX neo FILES ${src_idlib})

if(HARDLINK_GAME)
	if(BASE)
		if(D3XP)
			message(WARNING "HARDLINK_GAME can only link one game - building base, not d3xp (disable BASE for a d3xp build)")
		endif()
		list(APPEND src_core ${src_game})
		set(hardlinked_game_dir "game")
	elseif(D3XP)
		list(APPEND src_core ${src_d3xp})
		set(hardlinked_game_dir "d3xp")
	endif()
endif()

if(CORE)
	add_executable(dhewm3 WIN32 MACOSX_BUNDLE ${src_core} ${src_sys})
	source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX neo FILES ${src_core} ${src_sys})

	target_include_directories(dhewm3 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libs/imgui")
	target_link_libraries(dhewm3 PRIVATE idlib SDL3::SDL3 OpenAL::OpenAL)

	if(CURL_FOUND)
		target_link_libraries(dhewm3 PRIVATE CURL::libcurl)
	endif()

	if(HARDLINK_GAME)
		target_include_directories(dhewm3 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${hardlinked_game_dir}")
	else()
		target_compile_definitions(dhewm3 PRIVATE __DOOM_DLL__)
	endif()

	if(APPLE)
		set_target_properties(dhewm3 PROPERTIES
			MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/sys/osx/Info.plist")
		target_link_libraries(dhewm3 PRIVATE "-framework Carbon" "-framework Cocoa" "-framework IOKit")
	else()
		target_link_libraries(dhewm3 PRIVATE winmm iphlpapi wsock32 ole32)
		# launch dhewm3 (rather than ALL_BUILD) when hitting "run" in Visual Studio
		set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT dhewm3)
	endif()
endif()

# The game code is built as a shared library that the executable loads at runtime,
# unless HARDLINK_GAME is set, in which case it's already part of the executable.
function(add_game_library name sourcedir)
	add_library(${name} SHARED ${ARGN})
	source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} PREFIX neo FILES ${ARGN})
	target_link_libraries(${name} PRIVATE idlib)
	target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${sourcedir}")
	target_compile_definitions(${name} PRIVATE GAME_DLL)
	set_target_properties(${name} PROPERTIES
		PREFIX ""
		INSTALL_NAME_DIR "@executable_path")
endfunction()

if(BASE AND NOT HARDLINK_GAME)
	add_game_library(base game ${src_game})
endif()

if(D3XP AND NOT HARDLINK_GAME)
	add_game_library(d3xp d3xp ${src_d3xp})
	target_compile_definitions(d3xp PRIVATE _D3XP CTF)
endif()

if(is_multi_config)
	message(STATUS "Building dhewm3 for ${os} (build type is chosen at build time)")
else()
	message(STATUS "Building dhewm3 ${CMAKE_BUILD_TYPE} for ${os}")
endif()
if(APPLE)
	message(STATUS "  Architectures: ${CMAKE_OSX_ARCHITECTURES}")
endif()
