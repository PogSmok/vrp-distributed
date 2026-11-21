# -------------------------
# vrp-core: GPU algorithms and their CPU references
# -------------------------
add_library(vrp_core STATIC
    src/distance_matrix.cpp
    src/geo.cpp
    src/graph.cpp
    src/leg_router.cpp
    src/reference/dijkstra.cpp
    src/reference/snapping.cpp
    src/solution.cpp
)
add_library(vrp::core ALIAS vrp_core)

target_include_directories(vrp_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(vrp_core PUBLIC cxx_std_20)

vrp_enable_warnings(vrp_core)

if(VRP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
