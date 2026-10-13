# Direct2D UI target for the reimplementation (task-5).
#
# Usable two ways:
#   1. as a subdirectory of reimpl\CMakeLists.txt via  add_subdirectory(src/ui)
#   2. standalone:  cmake -S reimpl\src\ui -B reimpl\src\ui\.build
#
# It compiles the frozen engine sources directly so the UI exe carries the real
# measurement code, and links OS DLLs only (d2d1, dwrite, windowscodecs, winhttp).
cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(speedtest_ui LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "This project targets MSVC 2022 (cl.exe).")
endif()

set(UI_ENGINE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../http_client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../json.cpp
)

# task-11's share upload (impl-app owns share.h and share.cpp). It is picked up
# automatically the moment share.cpp exists, so the UI never has to pretend a
# link was published when there is no upload implementation to call.
set(UI_SHARE_SOURCES "")
set(UI_SHARE_DEFS "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../share.cpp")
    set(UI_SHARE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../share.cpp")
    set(UI_SHARE_DEFS ST_UI_HAVE_SHARE_IMPL=1)
    message(STATUS "impl-ui: share.cpp found - real share upload is linked")
else()
    message(STATUS "impl-ui: share.cpp missing - share reports an honest failure")
endif()

add_executable(speedtest-ui WIN32
    ui_main.cpp
    ui_app.cpp
    ui_draw.cpp
    ui_share.cpp
    ${UI_SHARE_SOURCES}
    ${UI_ENGINE_SOURCES}
)

target_include_directories(speedtest-ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_definitions(speedtest-ui PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    _CRT_SECURE_NO_WARNINGS
    ${UI_SHARE_DEFS}
)

target_compile_options(speedtest-ui PRIVATE /W3 /EHsc /permissive- /utf-8)

target_link_libraries(speedtest-ui PRIVATE
    d2d1 dwrite windowscodecs gdi32 user32 ole32
    winhttp ws2_32
    bcrypt
)

set_target_properties(speedtest-ui PROPERTIES
    OUTPUT_NAME "speedtest-ui"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)
