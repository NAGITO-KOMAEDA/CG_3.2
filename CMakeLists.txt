cmake_minimum_required(VERSION 3.20)
project(SponzaTessellation LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This Direct3D 11 lab requires Windows.")
endif()

add_executable(SponzaTessellation src/main.cpp)
target_compile_features(SponzaTessellation PRIVATE cxx_std_17)
target_compile_definitions(SponzaTessellation PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(SponzaTessellation PRIVATE d3d11 d3dcompiler dxgi)
set_property(TARGET SponzaTessellation PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
