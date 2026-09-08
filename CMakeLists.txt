cmake_minimum_required(VERSION 3.20)

project(CapturaClipA2
    VERSION 0.4.1
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
    src/app/MouseBindings.cpp
    src/app/Settings.cpp
    src/app/Shortcuts.cpp
    src/capture/DibBuffer.cpp
    src/capture/ScreenSnapshot.cpp
    src/capture/WindowPicker.cpp
    src/doc/Document.cpp
    src/doc/History.cpp
    src/doc/Selection.cpp
    src/io/AutoSave.cpp
    src/io/Clipboard.cpp
    src/io/ImageCodec.cpp
    src/io/ImageOps.cpp
    src/overlay/SelectionOverlay.cpp
    src/render/D2DContext.cpp
    src/render/Renderer.cpp
    src/render/SelectionGeometry.cpp
    src/ui/ClipWindow.cpp
    src/ui/ColorPopup.cpp
    src/ui/ColorPreview.cpp
    src/ui/ConcatDialog.cpp
    src/ui/DecorPanel.cpp
    src/ui/FontPicker.cpp
    src/ui/RotateDialog.cpp
    src/ui/SettingsDialog.cpp
    src/ui/ToolCursors.cpp
    src/util/Dpi.cpp
    src/util/NameFormat.cpp
)

set(APP_HEADERS
    src/app/MouseBindings.h
    src/app/Settings.h
    src/app/Shortcuts.h
    src/capture/DibBuffer.h
    src/capture/ScreenSnapshot.h
    src/capture/WindowPicker.h
    src/doc/Annotation.h
    src/doc/Document.h
    src/doc/History.h
    src/doc/Selection.h
    src/io/AutoSave.h
    src/io/Clipboard.h
    src/io/ImageCodec.h
    src/io/ImageOps.h
    src/overlay/SelectionOverlay.h
    src/render/D2DContext.h
    src/render/Renderer.h
    src/render/SelectionGeometry.h
    src/ui/ClipWindow.h
    src/ui/ColorPopup.h
    src/ui/ColorPreview.h
    src/ui/ConcatDialog.h
    src/ui/DecorPanel.h
    src/ui/FontPicker.h
    src/ui/RotateDialog.h
    src/res/Resources.h
    src/ui/SettingsDialog.h
    src/ui/ToolCursors.h
    src/tool/Tool.h
    src/tool/ToolState.h
    src/util/Dpi.h
    src/util/NameFormat.h
    src/util/Timing.h
    src/view/ViewState.h
)

# The version the shell shows in the file's properties comes from `project()`
# above, so there is one number to change at release time.
configure_file(src/res/Version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h" @ONLY)

add_executable(CapturaClipA2 WIN32
    ${APP_SOURCES}
    ${APP_HEADERS}
    src/app.manifest
    src/app.rc
)

target_include_directories(CapturaClipA2 PRIVATE
    src
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

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
    # Hardening. None of these fix a fault on their own; they raise what it
    # costs to turn one into something worse. /Qspectre is deliberately absent
    # -- it needs the Spectre-mitigated libraries, which are a separate install.
    /guard:cf
    /sdl
)

target_link_options(CapturaClipA2 PRIVATE
    /guard:cf
    /CETCOMPAT
    # Statically imported libraries are resolved before any code of ours runs,
    # so SetDefaultDllDirectories cannot reach them. 0x800 is
    # LOAD_LIBRARY_SEARCH_SYSTEM32, which is where every one of them lives.
    /DEPENDENTLOADFLAG:0x800
)

target_link_libraries(CapturaClipA2 PRIVATE
    d2d1
    dwrite
    windowscodecs
    dwmapi
    shcore
    ole32
    oleaut32
    shell32
    shlwapi
    comdlg32
    comctl32
    user32
    gdi32
    msimg32
    imm32
)

# Keep the source tree free of build artifacts.
set_target_properties(CapturaClipA2 PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:CapturaClipA2>"
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "src" FILES
    ${APP_SOURCES}
    ${APP_HEADERS}
)
