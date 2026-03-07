find_package(CURL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

file(GLOB_RECURSE ASYNCDOWNLOAD_LIBRARY_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
list(REMOVE_ITEM ASYNCDOWNLOAD_LIBRARY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")

add_library(asyncdownload STATIC ${ASYNCDOWNLOAD_LIBRARY_SOURCES})

target_include_directories(asyncdownload
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

file(GLOB LIB_INCLUDE_DIRS LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/libs/*/include")
foreach(inc_dir ${LIB_INCLUDE_DIRS})
    if(IS_DIRECTORY ${inc_dir})
        target_include_directories(asyncdownload PUBLIC ${inc_dir})
    endif()
endforeach()

target_link_libraries(asyncdownload
    PUBLIC
        CURL::libcurl
        nlohmann_json::nlohmann_json
)

target_compile_features(asyncdownload PUBLIC cxx_std_20)

add_executable(asyncdownload_cli main.cpp)
set_target_properties(asyncdownload_cli PROPERTIES OUTPUT_NAME "${PROJECT_NAME}")
target_link_libraries(asyncdownload_cli PRIVATE asyncdownload)
target_compile_features(asyncdownload_cli PRIVATE cxx_std_20)
