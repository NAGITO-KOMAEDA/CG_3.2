cmake_minimum_required(VERSION 3.24)

project(CG2 LANGUAGES CXX)

add_executable(CG2
    main.cpp
    RenderingSystem.cpp
    RenderingSystem.hpp
    GBuffer.cpp
    GBuffer.hpp
    Scene.cpp
    Scene.hpp
    Camera.hpp
    DxUtil.hpp
)

target_compile_features(CG2 PRIVATE cxx_std_20)
target_compile_definitions(CG2 PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(CG2 PRIVATE /W4 /permissive- /Zc:preprocessor)
target_link_libraries(CG2 PRIVATE
    d3d12
    dxgi
    dxguid
    d3dcompiler
    user32
    shell32
)

set_target_properties(CG2 PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:CG2>"
)

add_custom_command(TARGET CG2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:CG2>/shaders"
            "$<TARGET_FILE_DIR:CG2>/assets/Sponza-master"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/textures"
            "$<TARGET_FILE_DIR:CG2>/assets/Sponza-master/textures"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/GBuffer.hlsl"
            "$<TARGET_FILE_DIR:CG2>/shaders/GBuffer.hlsl"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/Lighting.hlsl"
            "$<TARGET_FILE_DIR:CG2>/shaders/Lighting.hlsl"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/sponza.obj"
            "$<TARGET_FILE_DIR:CG2>/assets/Sponza-master/sponza.obj"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/sponza.mtl"
            "$<TARGET_FILE_DIR:CG2>/assets/Sponza-master/sponza.mtl"
    COMMENT "Copying shaders and Sponza assets"
)
