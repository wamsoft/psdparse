cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_compile_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C>>:/utf-8>")
    add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/utf-8>")
    add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/Zc:__cplusplus>")
    project(psdparse LANGUAGES CXX)
endif()

# Python バインディング (オプション、別プリセット / pip で ON)
option(PSDPARSE_BUILD_PYTHON "Build pybind11 Python bindings" OFF)
if(PSDPARSE_BUILD_PYTHON)
    # 拡張モジュールは libpsdparse.a (および取得した zlib) を .so にリンクするため、
    # 静的オブジェクトはすべて位置独立コード (PIC) でビルドする必要がある。
    # add_subdirectory(psdparse) より前に設定し、配下の全ターゲットへ波及させる。
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# C++ ライブラリ本体 (吉里吉里非依存、pure C++17)
add_subdirectory(psdparse)

if(PSDPARSE_BUILD_PYTHON)
    add_subdirectory(python)
endif()
