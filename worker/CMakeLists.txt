# -------------------------
# vrp-worker: runs tasks on one GPU for the orchestrator (ADR 0024)
# -------------------------
find_package(Threads REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)

# The contract (ADR 0008), generated at build time.
set(proto_root ${PROJECT_SOURCE_DIR}/proto)
set(generated ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${generated})

add_library(vrp_proto STATIC ${proto_root}/vrp/v1/vrp.proto)
target_link_libraries(vrp_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
# SYSTEM keeps the generated headers out of our warnings and clang-tidy.
target_include_directories(vrp_proto SYSTEM PUBLIC ${generated})
protobuf_generate(
    TARGET vrp_proto
    LANGUAGE cpp
    IMPORT_DIRS ${proto_root}
    PROTOC_OUT_DIR ${generated}
)
protobuf_generate(
    TARGET vrp_proto
    LANGUAGE grpc
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
    IMPORT_DIRS ${proto_root}
    PROTOC_OUT_DIR ${generated}
)

add_library(vrp_worker STATIC
    src/config.cpp
    src/gpu_monitor.cpp
    src/graph_cache.cpp
    src/hashing.cpp
    src/plan_router.cpp
    src/session_client.cpp
    src/session_state.cpp
    src/task_runner.cpp
    src/telemetry.cpp
    src/threaded_for.cpp
    src/transfer.cpp
)
target_include_directories(vrp_worker
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vrp_worker
    PUBLIC
        vrp::core_gpu
        vrp_proto
        prometheus-cpp::pull
        Threads::Threads
    PRIVATE
        CURL::libcurl
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
        ${CMAKE_DL_LIBS}  # loads NVML at run time
)
# curl.h includes windows.h, whose `min` and `max` macros break gRPC's headers.
target_compile_definitions(vrp_worker PUBLIC
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
    $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
)
vrp_enable_warnings(vrp_worker)

add_executable(vrp-worker src/main.cpp)
target_link_libraries(vrp-worker
    PRIVATE
        vrp_worker
        vrp_build_version
        CURL::libcurl
)
target_compile_definitions(vrp-worker PRIVATE
    # For std::getenv, which MSVC deprecates.
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
vrp_enable_warnings(vrp-worker)

if(VRP_BUILD_TESTS)
    include(GoogleTest)

    add_executable(vrp_worker_tests
        tests/test_graph_cache.cpp
        tests/test_plan_router.cpp
        tests/test_session_state.cpp
        tests/test_threaded_for.cpp
    )
    target_link_libraries(vrp_worker_tests
        PRIVATE
            vrp_worker
            GTest::gtest_main
    )
    vrp_enable_warnings(vrp_worker_tests)
    gtest_discover_tests(vrp_worker_tests
        DISCOVERY_TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
    )
endif()
