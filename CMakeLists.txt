cmake_minimum_required(VERSION 3.20)

project(CapturaClipA2
    VERSION 0.1.0
    DESCRIPTION "Pin a region of the screen on top of everything else"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Link the CRT statically so the binary runs without a redistributable installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(APP_SOURCES
    src/main.cpp
    src/util/Dpi.cpp
)

set(APP_HEADERS
    src/util/Dpi.h
)

add_executable(CapturaClipA2 WIN32
    ${APP_SOURCES}
    ${APP_HEADERS}
    src/app.manifest
)

target_include_directories(CapturaClipA2 PRIVATE src)

target_compile_definitions(CapturaClipA2 PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    STRICT
)

target_compile_options(CapturaClipA2 PRIVATE
    /W4
    /permissive-
    /utf-8
    /Zc:__cplusplus
)

target_link_libraries(CapturaClipA2 PRIVATE
    d2d1
    dwrite
    windowscodecs
    dwmapi
    shcore
    user32
    gdi32
)

# Keep the source tree free of build artifacts.
set_target_properties(CapturaClipA2 PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:CapturaClipA2>"
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "src" FILES
    ${APP_SOURCES}
    ${APP_HEADERS}
)
