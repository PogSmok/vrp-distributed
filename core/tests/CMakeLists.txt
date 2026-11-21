include(GoogleTest)
find_package(Threads REQUIRED)

add_executable(vrp_core_tests
    test_distance_matrix.cpp
    test_geo.cpp
    test_graph.cpp
    test_graphs.cpp
    test_leg_router.cpp
    test_solution.cpp
    test_status.cpp
    test_status_macros.cpp
    reference/test_dijkstra.cpp
    reference/test_snapping.cpp
)

target_include_directories(vrp_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(vrp_core_tests
    PRIVATE
        vrp::core
        GTest::gtest_main
        Threads::Threads
)

vrp_enable_warnings(vrp_core_tests)

gtest_discover_tests(vrp_core_tests
    DISCOVERY_TIMEOUT 60
    DISCOVERY_MODE PRE_TEST
)
