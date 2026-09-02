# Dear ImGui - the copy this tree already vendors, built as the target
# imgui-eacp asks find_package(ImGui) for.
#
# **There has to be exactly one ImGui in the binary**, and this is what makes
# that true. imgui-eacp fetches v1.92.5 with CPM and builds an `imgui` target
# from it; dhewm3 has vendored 1.92.5 under neo/libs/imgui since long before this
# port, with dhewm3's own imconfig.h beside it, and Dhewm3SettingsMenu.cpp
# includes "../libs/imgui/imgui.h" by relative path. Two copies would be two sets
# of statics behind one `ImGui::` namespace - a context created against one and
# read through the other - and nothing about it would fail at link time.
#
# So the vendored directory stays the source of truth and imgui-eacp is handed
# it. This module wins over imgui-eacp's own FindImGui.cmake because the root
# CMakeLists appends this directory to CMAKE_MODULE_PATH before imgui-eacp
# appends its own, and find_package searches that list in order - so the
# download never happens.

if(NOT TARGET imgui)
	set(_dhewm3_imgui_dir "${CMAKE_CURRENT_LIST_DIR}/../neo/libs/imgui")

	if(NOT EXISTS "${_dhewm3_imgui_dir}/imgui.cpp")
		message(FATAL_ERROR
			"FindImGui: no vendored Dear ImGui at ${_dhewm3_imgui_dir}")
	endif()

	add_library(imgui STATIC
		"${_dhewm3_imgui_dir}/imgui.cpp"
		"${_dhewm3_imgui_dir}/imgui_draw.cpp"
		"${_dhewm3_imgui_dir}/imgui_tables.cpp"
		"${_dhewm3_imgui_dir}/imgui_widgets.cpp"

		# ImGui::ShowDemoWindow lives here, and dhewm3's settings menu opens it:
		# `dhewm3Settings` has a button for it, D3_ImGuiWin_Demo.
		"${_dhewm3_imgui_dir}/imgui_demo.cpp")

	# SYSTEM for the reason imgui-eacp's own module gives - ImGui does not build
	# clean under -Wall -Wextra -Wpedantic - and PUBLIC so that <imgui.h> resolves
	# in imgui-eacp's headers as well as in this tree's own files. neo/ still
	# reaches it by relative path, which is what keeps imconfig.h in effect.
	target_include_directories(imgui SYSTEM PUBLIC "${_dhewm3_imgui_dir}")

	# ImGui's own requirement, and not the engine's: neo/ pins C++11 for the
	# Doom 3 code, and this target sets its own standard in its own right.
	target_compile_features(imgui PUBLIC cxx_std_20)

	set_target_properties(imgui PROPERTIES FOLDER Dependencies)

	add_library(imgui::imgui ALIAS imgui)

	unset(_dhewm3_imgui_dir)
endif()

set(ImGui_FOUND TRUE)
