cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)
project(AmazonMusicRPC VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(AmazonMusicRPC WIN32
    src/app.cpp
    src/worker.cpp
    src/diagnostics.cpp
    src/config.cpp
    src/util.cpp
    src/media.cpp
    src/amazon_log.cpp
    src/http.cpp
    src/artwork.cpp
    src/discord_ipc.cpp
)

target_compile_features(AmazonMusicRPC PRIVATE cxx_std_20)
target_compile_definitions(AmazonMusicRPC PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

if(MSVC)
    target_compile_options(AmazonMusicRPC PRIVATE /utf-8 /W4 /permissive- /EHsc)
else()
    message(FATAL_ERROR "This project requires MSVC (C++/WinRT).")
endif()

target_include_directories(AmazonMusicRPC PRIVATE src)

target_link_libraries(AmazonMusicRPC PRIVATE
    windowsapp
    runtimeobject
    ole32
    oleaut32
    shell32
    advapi32
    winhttp
    user32
    comctl32
    gdi32
)

set_target_properties(AmazonMusicRPC PROPERTIES
    OUTPUT_NAME "AmazonMusicRPC"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
