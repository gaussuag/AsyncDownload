add_executable(${PROJECT_NAME}_tests 
    main_test.cpp
)

target_link_vcpkg_deps(${PROJECT_NAME}_tests)

target_link_libraries(${PROJECT_NAME}_tests 
    PRIVATE
    gtest_main
    gmock
)

target_include_directories(${PROJECT_NAME}_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

file(GLOB LIB_INCLUDE_DIRS LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/libs/*/include")
foreach(inc_dir ${LIB_INCLUDE_DIRS})
    if(IS_DIRECTORY ${inc_dir})
        target_include_directories(${PROJECT_NAME}_tests PRIVATE ${inc_dir})
    endif()
endforeach()

include(GoogleTest)
gtest_discover_tests(${PROJECT_NAME}_tests)
