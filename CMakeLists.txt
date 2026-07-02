cmake_minimum_required(VERSION 3.16)
project(focus-event CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -O2
)

# Shared static library with all the engine internals: KDL parser, niri JSON
# codec, epoll wrapper, subprocess helpers, UDS framing, rule engine.
add_library(focus-event-lib STATIC
    src/lib/kdl.cpp
    src/lib/config.cpp
    src/lib/niri.cpp
    src/lib/epoll.cpp
    src/lib/subprocess.cpp
    src/lib/uds.cpp
    src/lib/engine.cpp
)
target_include_directories(focus-event-lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

# The privileged system service: listens on a UDS, fork+setsid+execs commands
# from peers it has cleared via SO_PEERCRED.
add_executable(focus-event-executor
    src/executor_main.cpp
)
target_link_libraries(focus-event-executor PRIVATE focus-event-lib)

# The user-space half: spawned by niri, reads config + event-stream, sends
# SPAWN frames to the executor over UDS.
add_executable(focus-event-trigger
    src/trigger_main.cpp
)
target_link_libraries(focus-event-trigger PRIVATE focus-event-lib)

install(TARGETS focus-event-executor focus-event-trigger RUNTIME DESTINATION bin)
