cmake_minimum_required(VERSION 3.22.1)
project(blone LANGUAGES CXX)

add_library(blone SHARED
        native-lib.cpp
        renderer/Renderer.cpp
        core/Mesh.cpp
        core/Scene.cpp)

target_include_directories(blone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(blone PRIVATE cxx_std_17)
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
