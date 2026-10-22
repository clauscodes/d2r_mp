cmake_minimum_required(VERSION 3.29)
project(d2r_rooms_plugin LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static runtime: the DLL needs no Visual C++ redistributable on the player's PC.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# D2RLoader Plugin SDK 0.3.0 (MIT, see sdk/LICENSE), headers only.
add_library(D2RLPlugin INTERFACE)
target_include_directories(D2RLPlugin INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include")

if(MSVC)
	add_compile_options(/W4 /permissive- /EHsc /utf-8)
endif()

# Everything without D2RLoader or Windows calls, shared by the plugin and the tests.
add_library(d2r_rooms_core STATIC src/json.cpp src/board.cpp src/layout.cpp)
target_include_directories(d2r_rooms_core PUBLIC src)

add_library(d2r_rooms SHARED src/plugin.cpp src/d2r_rooms.rc)
target_link_libraries(d2r_rooms PRIVATE d2r_rooms_core D2RLPlugin user32)
# D2RLoader pairs d2rl-<plugin id>.dll with its id.
set_target_properties(d2r_rooms PROPERTIES OUTPUT_NAME "d2rl-d2r-rooms")

enable_testing()
add_executable(d2r_rooms_tests tests/tests.cpp)
target_link_libraries(d2r_rooms_tests PRIVATE d2r_rooms_core)
add_test(NAME d2r_rooms_tests COMMAND d2r_rooms_tests)
