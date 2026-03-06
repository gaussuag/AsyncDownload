add_executable(${PROJECT_NAME} main.cpp)

target_link_vcpkg_deps(${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} 
    PUBLIC 
        ${CMAKE_SOURCE_DIR}/include
)

file(GLOB LIB_INCLUDE_DIRS LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/libs/*/include")
foreach(inc_dir ${LIB_INCLUDE_DIRS})
    if(IS_DIRECTORY ${inc_dir})
        target_include_directories(${PROJECT_NAME} PUBLIC ${inc_dir})
    endif()
endforeach()

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)
