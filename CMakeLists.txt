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

add_executable(focus-event
    src/main.cpp
    src/kdl.cpp
    src/config.cpp
    src/niri.cpp
    src/epoll.cpp
    src/subprocess.cpp
    src/engine.cpp
)

target_include_directories(focus-event PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(focus-event PRIVATE)

install(TARGETS focus-event RUNTIME DESTINATION bin)
