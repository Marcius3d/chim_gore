cmake_minimum_required(VERSION 3.21)

project(CHIMGore VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

find_package(CommonLibSSE CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME}
    NAME "CHIMGore"
    AUTHOR "Marcius3d"
    USE_ADDRESS_LIBRARY
    SOURCES src/main.cpp
)

target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

target_link_libraries(${PROJECT_NAME} PRIVATE winhttp version)

target_compile_definitions(${PROJECT_NAME}
    PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /Zc:preprocessor /EHsc /bigobj /utf-8)
endif ()
