cmake_minimum_required(VERSION 3.20)

project(ParticleLab LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ParticleLab uses Direct3D 11 and can only be built on Windows")
endif()

add_executable(ParticleLab WIN32
    src/main.cpp
    src/Renderer.cpp
    src/Renderer.h
    src/ObjLoader.cpp
    src/ObjLoader.h
    src/MathTypes.h
)

target_compile_features(ParticleLab PRIVATE cxx_std_20)
target_compile_definitions(ParticleLab PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ParticleLab PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(ParticleLab PRIVATE d3d11 dxgi d3dcompiler)

add_custom_command(TARGET ParticleLab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
            "$<TARGET_FILE_DIR:ParticleLab>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:ParticleLab>/assets"
)

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT ParticleLab)
set_target_properties(ParticleLab PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:ParticleLab>"
)
