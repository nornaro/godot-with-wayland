cmake_minimum_required(VERSION 3.10...3.17)
project(godot_wayland LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WLROOTS REQUIRED wlroots-0.19)
pkg_check_modules(X11 REQUIRED x11 xext xtst xdamage)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/godot-cpp)

add_library(godot_wayland SHARED)

target_sources(
	${PROJECT_NAME}
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/src/compositor.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/src/register_types.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/src/wayland_node.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/src ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/protocols ${WLROOTS_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE godot-cpp ${WLROOTS_LINK_LIBRARIES})

set_target_properties(
	${PROJECT_NAME}
	PROPERTIES
		CXX_EXTENSIONS OFF
		CXX_VISIBILITY_PRESET ${GODOTCPP_SYMBOL_VISIBILITY}
		POSITION_INDEPENDENT_CODE ON
		BUILD_RPATH_USE_ORIGIN ON
		LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/bin>"
		RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/bin>"
		PREFIX "lib"
		OUTPUT_NAME "godot_wayland"
		FOLDER "godot_wayland"
)

# Gamescope extension: dedicated GameScope node + Gamescope-aware backend.
# Sources live in the addons/godot_backend addon; output goes to addons/bin.
add_library(godot_gamescope SHARED)

target_sources(
	godot_gamescope
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/src/compositor.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/src/compositor_gamescope.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/src/gamescope_node.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/src/register_gamescope_types.cpp
)

target_include_directories(godot_gamescope PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/src
	${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/protocols
	${WLROOTS_INCLUDE_DIRS}
)

target_link_libraries(godot_gamescope PRIVATE godot-cpp ${WLROOTS_LINK_LIBRARIES})

set_target_properties(
	godot_gamescope
	PROPERTIES
		CXX_EXTENSIONS OFF
		CXX_VISIBILITY_PRESET ${GODOTCPP_SYMBOL_VISIBILITY}
		POSITION_INDEPENDENT_CODE ON
		BUILD_RPATH_USE_ORIGIN ON
		LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/bin>"
		RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/bin>"
		PREFIX "lib"
		OUTPUT_NAME "godot_gamescope"
		FOLDER "godot_backend"
)

# X11WM extension: pure-X11 window manager (TinyWM/mwm style) node.
add_library(godot_x11wm SHARED)

target_sources(
	godot_x11wm
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/src/x11_wm.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/src/x11wm_node.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/src/register_x11wm_types.cpp
)

target_include_directories(godot_x11wm PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/src
	${X11_INCLUDE_DIRS}
)

target_link_libraries(godot_x11wm PRIVATE godot-cpp ${X11_LINK_LIBRARIES})

set_target_properties(
	godot_x11wm
	PROPERTIES
		CXX_EXTENSIONS OFF
		CXX_VISIBILITY_PRESET ${GODOTCPP_SYMBOL_VISIBILITY}
		POSITION_INDEPENDENT_CODE ON
		BUILD_RPATH_USE_ORIGIN ON
		LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/bin>"
		RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/../addons/godot_backend/bin>"
		PREFIX "lib"
		OUTPUT_NAME "godot_x11wm"
		FOLDER "godot_backend"
)
