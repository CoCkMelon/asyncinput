# Agent: Agent Mode, Date: 2025-08-16, Observation: Initial CMake build config for libasyncinput and examples
cmake_minimum_required(VERSION 3.13)
project(asyncinput C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Options
option(ASYNCINPUT_BUILD_SHARED "Build shared library" ON)
option(ASYNCINPUT_BUILD_STATIC "Build static library" ON)
option(ASYNCINPUT_BUILD_EXAMPLES "Build example programs" ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Include dirs
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Sources
set(ASYNCINPUT_SRC
    src/libasyncinput.c
)

# Libraries
find_package(Threads REQUIRED)

if(ASYNCINPUT_BUILD_SHARED)
    add_library(asyncinput_shared SHARED ${ASYNCINPUT_SRC})
    target_link_libraries(asyncinput_shared PRIVATE Threads::Threads)
    set_target_properties(asyncinput_shared PROPERTIES OUTPUT_NAME asyncinput)
endif()

if(ASYNCINPUT_BUILD_STATIC)
    add_library(asyncinput_static STATIC ${ASYNCINPUT_SRC})
    target_link_libraries(asyncinput_static PRIVATE Threads::Threads)
    set_target_properties(asyncinput_static PROPERTIES OUTPUT_NAME asyncinput)
endif()

# Examples
if(ASYNCINPUT_BUILD_EXAMPLES)
    add_executable(read_keys examples/read_keys.c)
    if(ASYNCINPUT_BUILD_SHARED)
        target_link_libraries(read_keys PRIVATE asyncinput_shared)
    else()
        target_link_libraries(read_keys PRIVATE asyncinput_static)
    endif()

    add_executable(callback_demo examples/callback_demo.c)
    if(ASYNCINPUT_BUILD_SHARED)
        target_link_libraries(callback_demo PRIVATE asyncinput_shared)
    else()
        target_link_libraries(callback_demo PRIVATE asyncinput_static)
    endif()

    add_executable(benchmark_asyncinput examples/benchmark_asyncinput.c)
    if(ASYNCINPUT_BUILD_SHARED)
        target_link_libraries(benchmark_asyncinput PRIVATE asyncinput_shared Threads::Threads)
    else()
        target_link_libraries(benchmark_asyncinput PRIVATE asyncinput_static Threads::Threads)
    endif()

    add_executable(mouse_demo examples/mouse_demo.c)
    if(ASYNCINPUT_BUILD_SHARED)
        target_link_libraries(mouse_demo PRIVATE asyncinput_shared Threads::Threads)
    else()
        target_link_libraries(mouse_demo PRIVATE asyncinput_static Threads::Threads)
    endif()

    # SDL3-based example (optional): sdl3_demo
    find_package(SDL3 QUIET)
    if(SDL3_FOUND)
        add_executable(sdl3_demo examples/sdl3_demo.c)
        target_link_libraries(sdl3_demo PRIVATE SDL3::SDL3 Threads::Threads)

        add_executable(sdl3_asyncinput examples/sdl3_asyncinput.c)
        if(ASYNCINPUT_BUILD_SHARED)
            target_link_libraries(sdl3_asyncinput PRIVATE SDL3::SDL3 asyncinput_shared Threads::Threads)
        else()
            target_link_libraries(sdl3_asyncinput PRIVATE SDL3::SDL3 asyncinput_static Threads::Threads)
        endif()
    else()
        message(STATUS "SDL3 not found; sdl3_demo and sdl3_asyncinput will not be built")
    endif()
endif()

