# -------------------------
# vrp-bench: runs vrp-core on files and times each phase (ADR 0023)
# -------------------------

# Reading input files; built without CUDA, so that its tests run everywhere.
add_library(vrp_bench_files STATIC
    src/cvrplib_file.cpp
    src/problem_file.cpp
)
target_include_directories(vrp_bench_files
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vrp_bench_files
    PUBLIC
        vrp::core
    PRIVATE
        nlohmann_json::nlohmann_json
)
vrp_enable_warnings(vrp_bench_files)

if(VRP_ENABLE_CUDA)
    add_executable(vrp-bench src/main.cpp)
    target_link_libraries(vrp-bench
        PRIVATE
            vrp::core_gpu
            vrp_bench_files
    )
    vrp_enable_warnings(vrp-bench)
endif()

if(VRP_BUILD_TESTS)
    include(GoogleTest)

    add_executable(vrp_bench_tests
        tests/test_cvrplib_file.cpp
        tests/test_problem_file.cpp
    )
    target_link_libraries(vrp_bench_tests
        PRIVATE
            vrp_bench_files
            GTest::gtest_main
    )
    vrp_enable_warnings(vrp_bench_tests)
    gtest_discover_tests(vrp_bench_tests
        DISCOVERY_TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
    )

    if(VRP_ENABLE_CUDA)
        # The whole command on the spec's golden graph (ADR 0027).
        add_test(NAME VrpBench.DistancesMatchTheReferenceOnTheGoldenGraph
            COMMAND vrp-bench distances
                --graph ${PROJECT_SOURCE_DIR}/docs/formats/golden-v1/graph.bin
                --problem ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden_problem.json
                --check
        )
        set_tests_properties(VrpBench.DistancesMatchTheReferenceOnTheGoldenGraph
            PROPERTIES
                LABELS gpu
                PASS_REGULAR_EXPRESSION "matches the CPU reference"
        )

        # Solving the same problem end to end; the result is checked by
        # vrp-bench itself (ADR 0025's checks).
        add_test(NAME VrpBench.SolvesTheGoldenProblem
            COMMAND vrp-bench solve
                --graph ${PROJECT_SOURCE_DIR}/docs/formats/golden-v1/graph.bin
                --problem ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden_problem.json
                --seconds 1
        )
        set_tests_properties(VrpBench.SolvesTheGoldenProblem
            PROPERTIES
                LABELS gpu
                PASS_REGULAR_EXPRESSION "best: 1 routes"
        )
    endif()
endif()
