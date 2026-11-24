cmake_minimum_required(VERSION 3.25)

# The worker's dependencies come from vcpkg, built once in release mode, for
# the worker and for the tools that generate its code (ADR 0019). Set before
# `project`, which runs vcpkg's toolchain file.
if(VRP_BUILD_WORKER)
    if(CMAKE_HOST_WIN32)
        set(vrp_triplet x64-windows-static-md-release)
    else()
        set(vrp_triplet x64-linux-release)
    endif()
    set(VCPKG_TARGET_TRIPLET ${vrp_triplet} CACHE STRING "")
    set(VCPKG_HOST_TRIPLET ${vrp_triplet} CACHE STRING "")
endif()

project(vrp_distributed
    VERSION 0.1.0
    DESCRIPTION "Distributed GPU solver for the Vehicle Routing Problem"
    LANGUAGES CXX
)

# -------------------------
# C++ standard
# -------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VRP_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
option(VRP_ENABLE_CUDA "Build the GPU kernels; needs CUDA 12.8 or a later 12.x" OFF)
# Without CUDA, the worker computes on the CPU instead (ADR 0037).
option(VRP_BUILD_WORKER "Build vrp-worker; needs vcpkg" OFF)

# -------------------------
# CUDA (ADR 0019)
# -------------------------
if(VRP_ENABLE_CUDA)
    # nvcc on Windows compiles host code with MSVC, so C++ must use it too.
    if(WIN32 AND NOT MSVC)
        message(FATAL_ERROR
            "The CUDA build on Windows needs MSVC; configure from an x64 "
            "Native Tools prompt for Visual Studio 2022.")
    endif()
    # Native code for the GTX 1060 (6.1), the oldest worker GPU, and the
    # RTX 5050 (12.0); compute_61 PTX runs on every GPU in between.
    # Found before enable_language, whose compiler check fails with a raw nvcc
    # error on a toolkit that lacks one of the architectures.
    find_package(CUDAToolkit REQUIRED)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        # 12.8 is the first toolkit with architecture 120; 13 drops 61.
        if(CUDAToolkit_VERSION VERSION_LESS 12.8 OR
                CUDAToolkit_VERSION VERSION_GREATER_EQUAL 13)
            message(FATAL_ERROR
                "CUDA ${CUDAToolkit_VERSION} cannot build for both the GTX "
                "1060 and the RTX 5050; use 12.8 or a later 12.x (ADR 0019), "
                "or set CMAKE_CUDA_ARCHITECTURES for your GPU.")
        endif()
        set(CMAKE_CUDA_ARCHITECTURES 61 120-real)
    endif()
    enable_language(CUDA)
    set(CMAKE_CUDA_STANDARD 20)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
    set(CMAKE_CUDA_EXTENSIONS OFF)
endif()

# Sources are UTF-8. CUDA's own headers need MSVC's conforming preprocessor.
add_compile_options(
    $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>
    $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CXX_COMPILER_ID:MSVC>>:-Xcompiler=/utf-8,/Zc:preprocessor>
)

# Strict warnings, treated as errors, for our own targets only. nvcc passes
# host-compiler flags through -Xcompiler; C4211 is in the stubs nvcc
# generates for kernels in anonymous namespaces.
function(vrp_enable_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4 /permissive->
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow>
        $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CXX_COMPILER_ID:MSVC>>:-Xcompiler=/W4,/wd4211>
        $<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-Xcompiler=-Wall,-Wextra>
    )
    set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
endfunction()

include(FetchContent)

# -------------------------
# nlohmann/json, for vrp-bench's input files and vrp-worker's configuration and
# logs; vrp-core does not use it
# -------------------------
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
)
FetchContent_MakeAvailable(nlohmann_json)

# -------------------------
# GoogleTest
# -------------------------
if(VRP_BUILD_TESTS)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

    # SYSTEM keeps GoogleTest's headers out of our warnings and clang-tidy.
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
        URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(googletest)

    # GoogleTest's own warnings are not ours to fix.
    foreach(dep gtest gtest_main)
        set_target_properties(${dep} PROPERTIES CXX_CLANG_TIDY "")
        target_compile_options(${dep} PRIVATE
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>
        )
    endforeach()

    enable_testing()
endif()

# -------------------------
# The git version of this build, which results record (ADR 0016, 0019)
# -------------------------
if(VRP_ENABLE_CUDA OR VRP_BUILD_WORKER)
    set(vrp_version_header ${CMAKE_BINARY_DIR}/generated/vrp/build_version.hpp)
    add_custom_target(vrp_build_version_file
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DFALLBACK=${PROJECT_VERSION}
            -DOUTPUT=${vrp_version_header}
            -P ${PROJECT_SOURCE_DIR}/cmake/build_version.cmake
        BYPRODUCTS ${vrp_version_header}
        VERBATIM
    )
    add_library(vrp_build_version INTERFACE)
    target_include_directories(vrp_build_version
        INTERFACE ${CMAKE_BINARY_DIR}/generated
    )
    add_dependencies(vrp_build_version vrp_build_version_file)
endif()

add_subdirectory(core)
add_subdirectory(bench)
if(VRP_BUILD_WORKER)
    add_subdirectory(worker)
endif()
