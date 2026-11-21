cmake_minimum_required(VERSION 3.25)

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

# Sources are UTF-8.
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# Strict warnings, treated as errors, for our own targets only.
function(vrp_enable_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow>
    )
    set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
endfunction()

# -------------------------
# GoogleTest
# -------------------------
if(VRP_BUILD_TESTS)
    include(FetchContent)

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

add_subdirectory(core)
