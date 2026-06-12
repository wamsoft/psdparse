cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_compile_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C>>:/utf-8>")
    add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/utf-8>")
    add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/Zc:__cplusplus>")
    project(psdparse LANGUAGES CXX)
endif()

# C++ ライブラリ本体 (吉里吉里非依存、pure C++17)
add_subdirectory(psdparse)

# Python バインディング (オプション、別プリセットで ON)
option(PSDPARSE_BUILD_PYTHON "Build pybind11 Python bindings" OFF)
if(PSDPARSE_BUILD_PYTHON)
    add_subdirectory(python)
endif()
