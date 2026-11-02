cmake_minimum_required(VERSION 3.18)
project(pipibibs CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(E ${CMAKE_CURRENT_SOURCE_DIR}/engine)
add_library(pipibibs SHARED
    pipibibs_jni.cpp
    ${E}/src/pipibibs_68k.cpp
    ${E}/runtime/runtime.cpp ${E}/runtime/bus.cpp ${E}/runtime/z80cpu.cpp
    ${E}/runtime/opl2.cpp ${E}/runtime/sound.cpp ${E}/runtime/video.cpp)
target_include_directories(pipibibs PRIVATE ${E}/runtime)
# the lifted code relies on sibling-call optimisation (see README): always optimise, also in the Gradle debug build
set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g0")
# The game code was written and tested with GCC. Make clang behave the same where C++ leaves room: wrapping integer overflow, no strict
# aliasing, zero-initialised locals.
target_compile_options(pipibibs PRIVATE -O2 -foptimize-sibling-calls -fwrapv -fno-strict-aliasing -fno-delete-null-pointer-checks -ftrivial-auto-var-init=zero -w)
target_link_libraries(pipibibs log)
