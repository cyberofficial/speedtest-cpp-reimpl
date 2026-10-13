cmake_minimum_required(VERSION 3.20)
project(speedtest_reimpl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT (/MT) so the Release EXE runs on a clean Windows 10/11 box.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT MSVC)
    message(FATAL_ERROR "This project targets MSVC 2022 (cl.exe). Configure with the Visual Studio 17 2022 generator.")
endif()

add_executable(speedtest
    src/main.cpp
    src/http_client.cpp
    src/json.cpp
    src/engine.cpp
    src/loopback_server.cpp
    src/share.cpp
)

target_include_directories(speedtest PRIVATE src)

target_compile_definitions(speedtest PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    _CRT_SECURE_NO_WARNINGS
)

target_compile_options(speedtest PRIVATE /W3 /EHsc /permissive- /utf-8)

target_link_libraries(speedtest PRIVATE winhttp ws2_32 bcrypt)

set_target_properties(speedtest PROPERTIES
    OUTPUT_NAME "speedtest"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

# impl-ui (task-5) owns src/ui/** and its own target definition.
add_subdirectory(src/ui)
