# -------------------------
# vrp-core: GPU algorithms and their CPU references
# -------------------------
add_library(vrp_core STATIC
    src/distance_matrix.cpp
    src/geo.cpp
    src/graph.cpp
    src/graph_file.cpp
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

# The GPU kernels, in their own target so that vrp_core builds and passes its
# tests without CUDA (ADR 0023).
if(VRP_ENABLE_CUDA)
    add_library(vrp_core_gpu STATIC
        src/gpu/device_buffer.cu
        src/gpu/device_graph.cu
        src/gpu/distance_batch.cu
    )
    add_library(vrp::core_gpu ALIAS vrp_core_gpu)

    target_link_libraries(vrp_core_gpu
        PUBLIC
            vrp::core
        PRIVATE
            CUDA::cudart_static
    )

    vrp_enable_warnings(vrp_core_gpu)
endif()

if(VRP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
