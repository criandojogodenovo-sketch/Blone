# Host-side build of the Blone core unit tests (no Android, no OpenGL).
# The core sources are compiled directly; Log.h compiles out on the host.
cmake_minimum_required(VERSION 3.16)
project(blone_core_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The core headers include "core/..." paths relative to app/src/main/cpp.
set(CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(blone_tests
        test_main.cpp
        test_halfedge.cpp
        test_loopcut.cpp
        test_ops.cpp
        test_transforms.cpp
        test_history_project.cpp
        test_render_gizmo.cpp
        ${CPP_DIR}/core/mesh/HalfEdgeMesh.cpp
        ${CPP_DIR}/core/mesh/Primitives.cpp
        ${CPP_DIR}/core/operations/Operations.cpp
        ${CPP_DIR}/core/history/History.cpp
        ${CPP_DIR}/core/render/RenderData.cpp
        ${CPP_DIR}/core/scene/SceneOps.cpp
        ${CPP_DIR}/core/project/Project.cpp)

target_include_directories(blone_tests PRIVATE ${CPP_DIR})
target_compile_options(blone_tests PRIVATE
        -Wall -Wextra -Wno-unused-parameter)

enable_testing()
add_test(NAME blone_core COMMAND blone_tests)
