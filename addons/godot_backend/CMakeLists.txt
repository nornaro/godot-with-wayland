cmake_minimum_required(VERSION 3.10...3.17)
project(godot_backend_extensions LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WLROOTS REQUIRED wlroots-0.19)
pkg_check_modules(X11 REQUIRED x11 xext xtst xdamage)

# godot-cpp bindings. Override GODOTCPP_SOURCE_DIR if you keep godot-cpp
# somewhere else, or set the standard godot-cpp options (GODOTCPP_TARGET,
# GODOTCPP_USE_STATIC_CPP, ...) on the command line.
set(GODOTCPP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../gdextension/thirdparty/godot-cpp" CACHE PATH "Path to the godot-cpp checkout")
add_subdirectory(${GODOTCPP_SOURCE_DIR})

# Core compositor sources shared by every backend extension.
set(GDWL_CORE_SOURCES
	src/compositor.cpp
	src/wayland_node.cpp
)

function(godot_backend_target TARGET_NAME EXTRA_SOURCES REGISTER_SOURCE)
	add_library(${TARGET_NAME} SHARED)
	target_sources(
		${TARGET_NAME}
		PRIVATE
			${GDWL_CORE_SOURCES}
			${EXTRA_SOURCES}
			${REGISTER_SOURCE}
	)
	target_include_directories(${TARGET_NAME} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/src
		${WLROOTS_INCLUDE_DIRS}
	)
	target_link_libraries(${TARGET_NAME} PRIVATE godot-cpp ${WLROOTS_LINK_LIBRARIES})
	set_target_properties(${TARGET_NAME} PROPERTIES
		CXX_EXTENSIONS OFF
		CXX_VISIBILITY_PRESET ${GODOTCPP_SYMBOL_VISIBILITY}
		POSITION_INDEPENDENT_CODE ON
		BUILD_RPATH_USE_ORIGIN ON
		LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/bin>"
		RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/bin>"
		PREFIX "lib"
		OUTPUT_NAME "${TARGET_NAME}"
		FOLDER "godot_backend"
	)
endfunction()

# Gamescope extension: registers the GameScope node and uses the
# Gamescope-aware backend factory from compositor_gamescope.cpp.
godot_backend_target(godot_gamescope src/compositor_gamescope.cpp src/register_gamescope_types.cpp)

# wm extension (mwm/TinyWM-optimized backend hints).
godot_backend_target(godot_wm src/compositor_wm.cpp src/register_types.cpp)

# x11wm extension: pure-X11 window manager (TinyWM/mwm style) node.
add_library(godot_x11wm SHARED)
target_sources(godot_x11wm PRIVATE src/x11_wm.cpp src/x11wm_node.cpp src/register_x11wm_types.cpp)
target_include_directories(godot_x11wm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${X11_INCLUDE_DIRS})
target_link_libraries(godot_x11wm PRIVATE godot-cpp ${X11_LINK_LIBRARIES})
set_target_properties(godot_x11wm PROPERTIES
	CXX_EXTENSIONS OFF
	CXX_VISIBILITY_PRESET ${GODOTCPP_SYMBOL_VISIBILITY}
	POSITION_INDEPENDENT_CODE ON
	BUILD_RPATH_USE_ORIGIN ON
	LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/bin>"
	RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/bin>"
	PREFIX "lib"
	OUTPUT_NAME "godot_x11wm"
	FOLDER "godot_backend"
)

# drm extension (KMS/DRM-optimized backend hints).
godot_backend_target(godot_drm src/compositor_drm.cpp src/register_types.cpp)
