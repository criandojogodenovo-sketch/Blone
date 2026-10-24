cmake_minimum_required(VERSION 3.22.1)
# Fase 4: C enabled for the vendored cgltf parser (third_party/cgltf).
project(blone LANGUAGES C CXX)

add_library(blone SHARED
        native-lib.cpp
        renderer/Renderer.cpp
        core/Mesh.cpp
        core/Scene.cpp
        core/brush/Brush.cpp
        core/io/MeshBuilder.cpp
        core/io/ObjLoader.cpp
        core/io/ObjExporter.cpp
        core/io/GltfLoader.cpp
        third_party/cgltf/cgltf.c
        core/mesh/HalfEdgeMesh.cpp
        core/mesh/Primitives.cpp
        core/operations/Operations.cpp
        core/history/History.cpp
        core/render/RenderData.cpp
        core/render/RenderMesh.cpp
        core/scene/SceneOps.cpp
        core/project/Project.cpp)

target_include_directories(blone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(blone PRIVATE cxx_std_17)
# The vendored cgltf is upstream C99 — keep warnings reasonable for it.
set_source_files_properties(third_party/cgltf/cgltf.c PROPERTIES
        COMPILE_OPTIONS "-Wno-unused-parameter")
target_compile_options(blone PRIVATE
        -O2
        -fexceptions
        -frtti
        -Wall
        -Wextra
        -Wno-unused-parameter)

find_library(log-lib log)

target_link_libraries(blone PRIVATE
        GLESv3
        ${log-lib})
