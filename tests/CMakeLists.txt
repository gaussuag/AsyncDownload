file(GLOB_RECURSE ASYNCDOWNLOAD_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
list(REMOVE_ITEM ASYNCDOWNLOAD_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/flow/packet_flow_fault_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/range/range_lifecycle_fault_test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/recovery/recovery_failure_test.cpp"
)

add_executable(asyncdownload_tests ${ASYNCDOWNLOAD_TEST_SOURCES})
set_target_properties(asyncdownload_tests PROPERTIES OUTPUT_NAME "${PROJECT_NAME}_tests")

target_link_libraries(asyncdownload_tests
    PRIVATE
        asyncdownload
        gtest_main
)

target_include_directories(asyncdownload_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)

file(GLOB LIB_INCLUDE_DIRS LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/libs/*/include")
foreach(inc_dir ${LIB_INCLUDE_DIRS})
    if(IS_DIRECTORY ${inc_dir})
        target_include_directories(asyncdownload_tests PRIVATE ${inc_dir})
    endif()
endforeach()

target_compile_features(asyncdownload_tests PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(asyncdownload_tests)

add_executable(asyncdownload_packet_flow_fault_tests
    flow/packet_flow_fault_test.cpp
    ${CMAKE_SOURCE_DIR}/src/core/error.cpp
    ${CMAKE_SOURCE_DIR}/src/flow/packet_flow.cpp
    ${CMAKE_SOURCE_DIR}/src/telemetry/telemetry_collector.cpp
    ${CMAKE_SOURCE_DIR}/src/telemetry/telemetry_session.cpp
)
target_compile_definitions(asyncdownload_packet_flow_fault_tests
    PRIVATE
        ASYNCDOWNLOAD_PACKET_FLOW_FAULT_TEST
)
target_include_directories(asyncdownload_packet_flow_fault_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(asyncdownload_packet_flow_fault_tests
    PRIVATE
        gtest_main
)
target_compile_features(
    asyncdownload_packet_flow_fault_tests
    PRIVATE
        cxx_std_20
)
gtest_discover_tests(asyncdownload_packet_flow_fault_tests)

add_executable(asyncdownload_range_lifecycle_fault_tests
    range/range_lifecycle_fault_test.cpp
    ${CMAKE_SOURCE_DIR}/src/core/block_bitmap.cpp
    ${CMAKE_SOURCE_DIR}/src/core/crc32.cpp
    ${CMAKE_SOURCE_DIR}/src/core/error.cpp
    ${CMAKE_SOURCE_DIR}/src/download/download_policy.cpp
    ${CMAKE_SOURCE_DIR}/src/download/range_scheduler.cpp
    ${CMAKE_SOURCE_DIR}/src/flow/packet_flow.cpp
    ${CMAKE_SOURCE_DIR}/src/metadata/metadata_store.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/persistence_thread.cpp
    ${CMAKE_SOURCE_DIR}/src/range/range_fact_slot.cpp
    ${CMAKE_SOURCE_DIR}/src/range/range_lifecycle.cpp
    ${CMAKE_SOURCE_DIR}/src/recovery/recovery_checkpoint.cpp
    ${CMAKE_SOURCE_DIR}/src/storage/file_writer.cpp
    ${CMAKE_SOURCE_DIR}/src/telemetry/telemetry_collector.cpp
    ${CMAKE_SOURCE_DIR}/src/telemetry/telemetry_session.cpp
)
target_compile_definitions(
    asyncdownload_range_lifecycle_fault_tests
    PRIVATE
        ASYNCDOWNLOAD_RANGE_LIFECYCLE_FAULT_TEST
)
target_include_directories(
    asyncdownload_range_lifecycle_fault_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)
file(GLOB RANGE_FAULT_LIB_INCLUDE_DIRS
    LIST_DIRECTORIES true
    "${CMAKE_SOURCE_DIR}/libs/*/include")
foreach(inc_dir ${RANGE_FAULT_LIB_INCLUDE_DIRS})
    if(IS_DIRECTORY ${inc_dir})
        target_include_directories(
            asyncdownload_range_lifecycle_fault_tests
            PRIVATE
                ${inc_dir})
    endif()
endforeach()
target_link_libraries(
    asyncdownload_range_lifecycle_fault_tests
    PRIVATE
        gtest_main
        CURL::libcurl
        nlohmann_json::nlohmann_json
)
target_compile_features(
    asyncdownload_range_lifecycle_fault_tests
    PRIVATE
        cxx_std_20
)
gtest_discover_tests(
    asyncdownload_range_lifecycle_fault_tests)

add_executable(asyncdownload_recovery_fault_tests
    recovery/recovery_failure_test.cpp
    ${CMAKE_SOURCE_DIR}/src/core/block_bitmap.cpp
    ${CMAKE_SOURCE_DIR}/src/core/crc32.cpp
    ${CMAKE_SOURCE_DIR}/src/core/error.cpp
    ${CMAKE_SOURCE_DIR}/src/download/download_policy.cpp
    ${CMAKE_SOURCE_DIR}/src/flow/packet_flow.cpp
    ${CMAKE_SOURCE_DIR}/src/metadata/metadata_store.cpp
    ${CMAKE_SOURCE_DIR}/src/persistence/persistence_thread.cpp
    ${CMAKE_SOURCE_DIR}/src/range/range_fact_slot.cpp
    ${CMAKE_SOURCE_DIR}/src/recovery/recovery_checkpoint.cpp
    ${CMAKE_SOURCE_DIR}/src/storage/file_writer.cpp
    ${CMAKE_SOURCE_DIR}/src/telemetry/telemetry_collector.cpp
    ${CMAKE_SOURCE_DIR}/src/telemetry/telemetry_session.cpp
)
target_compile_definitions(
    asyncdownload_recovery_fault_tests
    PRIVATE
        ASYNCDOWNLOAD_RECOVERY_FAULT_TEST
)
target_include_directories(
    asyncdownload_recovery_fault_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
)
file(GLOB RECOVERY_FAULT_LIB_INCLUDE_DIRS
    LIST_DIRECTORIES true
    "${CMAKE_SOURCE_DIR}/libs/*/include")
foreach(inc_dir ${RECOVERY_FAULT_LIB_INCLUDE_DIRS})
    if(IS_DIRECTORY ${inc_dir})
        target_include_directories(
            asyncdownload_recovery_fault_tests
            PRIVATE
                ${inc_dir})
    endif()
endforeach()
target_link_libraries(
    asyncdownload_recovery_fault_tests
    PRIVATE
        gtest_main
        nlohmann_json::nlohmann_json
)
target_compile_features(
    asyncdownload_recovery_fault_tests
    PRIVATE
        cxx_std_20
)
gtest_discover_tests(
    asyncdownload_recovery_fault_tests)

find_package(CURL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
