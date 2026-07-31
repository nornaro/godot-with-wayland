cmake_minimum_required(VERSION 3.10...3.17)
project(godot_wayland LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WLROOTS REQUIRED wlroots-0.19)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/godot-cpp)

add_library(godot_wayland SHARED)

target_sources(
	${PROJECT_NAME}
	PRIVATE
		src/compositor.cpp
		src/register_types.cpp
		src/wayland_node.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/protocols ${WLROOTS_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE godot-cpp ${WLROOTS_LINK_LIBRARIES})

set_target_properties(
	${PROJECT_NAME}
	PROPERTIES
		CXX_EXTENSIONS OFF
		CXX_VISIBILITY_PRESET ${GODOTCPP_SYMBOL_VISIBILITY}
		POSITION_INDEPENDENT_CODE ON
		BUILD_RPATH_USE_ORIGIN ON
		LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/bin>"
		RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_SOURCE_DIR}/bin>"
		PREFIX "lib"
		OUTPUT_NAME "godot_wayland"
		FOLDER "godot_wayland"
)
