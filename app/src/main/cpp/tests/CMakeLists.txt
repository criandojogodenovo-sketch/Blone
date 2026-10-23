# Host-side build of the Blone core unit tests (no Android, no OpenGL).
# The core sources are compiled directly; Log.h compiles out on the host.
cmake_minimum_required(VERSION 3.16)
# Fase 4: C enabled for the vendored cgltf parser.
project(blone_core_tests LANGUAGES C CXX)

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
        test_selection.cpp
        test_ui_modes.cpp
        test_brush.cpp
        test_io.cpp
        ${CPP_DIR}/core/brush/Brush.cpp
        ${CPP_DIR}/core/io/MeshBuilder.cpp
        ${CPP_DIR}/core/io/ObjLoader.cpp
        ${CPP_DIR}/core/io/ObjExporter.cpp
        ${CPP_DIR}/core/io/GltfLoader.cpp
        ${CPP_DIR}/third_party/cgltf/cgltf.c
        ${CPP_DIR}/core/mesh/HalfEdgeMesh.cpp
        ${CPP_DIR}/core/mesh/Primitives.cpp
        ${CPP_DIR}/core/operations/Operations.cpp
        ${CPP_DIR}/core/history/History.cpp
        ${CPP_DIR}/core/render/RenderData.cpp
        ${CPP_DIR}/core/render/RenderMesh.cpp
        ${CPP_DIR}/core/scene/SceneOps.cpp
        ${CPP_DIR}/core/project/Project.cpp
        test_readmode.cpp)

target_include_directories(blone_tests PRIVATE ${CPP_DIR})
# Fixture dir for the I/O tests (cube.glb lives in tests/data).
target_compile_definitions(blone_tests PRIVATE
        BLONE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_options(blone_tests PRIVATE
        -Wall -Wextra -Wno-unused-parameter)
set_source_files_properties(${CPP_DIR}/third_party/cgltf/cgltf.c PROPERTIES
        COMPILE_OPTIONS "-Wno-unused-parameter")

enable_testing()
add_test(NAME blone_core COMMAND blone_tests)
