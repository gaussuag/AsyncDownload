file(GLOB_RECURSE ASYNCDOWNLOAD_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
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
