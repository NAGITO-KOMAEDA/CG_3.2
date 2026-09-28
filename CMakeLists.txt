cmake_minimum_required(VERSION 3.24)
project(Lab07PostProcessing LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This laboratory uses Win32 and DirectX 11.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG v5.4.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(assimp)

add_executable(Lab07PostProcessing WIN32
    src/main.cpp
    src/SceneModel.cpp
    src/SceneModel.h
    src/TextureLoader.cpp
    src/TextureLoader.h
)

target_compile_definitions(Lab07PostProcessing PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

if(MSVC)
    target_compile_options(Lab07PostProcessing PRIVATE /W4 /permissive- /utf-8)
endif()

target_link_libraries(Lab07PostProcessing PRIVATE
    assimp::assimp d3d11 dxgi d3dcompiler)

add_custom_command(TARGET Lab07PostProcessing POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
            "$<TARGET_FILE_DIR:Lab07PostProcessing>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:Lab07PostProcessing>/assets"
    COMMENT "Copying shaders and Sponza assets"
)

