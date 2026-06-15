cmake_minimum_required(VERSION 3.16)

# UTF-8 source
add_compile_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C>>:/utf-8>")
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/utf-8>")
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/Zc:__cplusplus>")

set(PROJECT_NAME psdparse)
set(PROJECT_VERSION 1.00)

project(${PROJECT_NAME} VERSION ${PROJECT_VERSION} LANGUAGES CXX)

# zlib: prefer a system / toolchain-provided zlib; otherwise vendor it via
# FetchContent so no external package manager (vcpkg etc.) is required.
# (Linux/macOS usually have system zlib; Windows falls back to the fetch.)
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    message(STATUS "psdparse: system zlib not found — fetching zlib 1.3.1 source")
    include(FetchContent)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)   # don't install vendored zlib into the wheel
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(zlib
        URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23)
    FetchContent_MakeAvailable(zlib)
    # zlib's CMake exposes `zlibstatic` (static) + `zlib` (shared). Normalise to
    # the ZLIB::ZLIB target our code links, and expose its headers (zlib.h plus
    # the generated zconf.h in the binary dir).
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
    endif()
    target_include_directories(zlibstatic PUBLIC
        $<BUILD_INTERFACE:${zlib_SOURCE_DIR}>
        $<BUILD_INTERFACE:${zlib_BINARY_DIR}>)
endif()

add_library(${PROJECT_NAME} STATIC
	bmp.cpp
	psddesc.cpp
	psdfile.cpp
	psdimage.cpp
	psdlayer.cpp
	psdparse.h
	psdparse.cpp
	psdresource.cpp
	psdwrite.cpp
)

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)

target_include_directories(${PROJECT_NAME} PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${PROJECT_NAME} PUBLIC
	ZLIB::ZLIB
)

# スタンドアロン CLI (PSD をロードしてヘッダを表示する smoke test 用)
option(PSDPARSE_BUILD_CLI "Build psdparse standalone CLI for smoke testing" OFF)
if(PSDPARSE_BUILD_CLI)
	add_executable(psdparse_cli psd_cli.cpp)
	target_link_libraries(psdparse_cli PRIVATE psdparse)
endif()
