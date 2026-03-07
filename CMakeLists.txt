cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED VCPKG_ROOT)
    set(VCPKG_ROOT "$ENV{VCPKG_ROOT}")
endif()

if(VCPKG_ROOT AND EXISTS "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain")
    set(VCPKG_MANIFEST_MODE ON)
endif()

project(AsyncDownload VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive-)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -pedantic)
endif()

include(cmake/CompilerOptions.cmake)

if(EXISTS ${CMAKE_SOURCE_DIR}/cmake/VcpkgDeps.cmake)
    include(cmake/VcpkgDeps.cmake)
endif()

#### AUTO-Generated: libs start ####
if(EXISTS ${CMAKE_SOURCE_DIR}/libs/googletest/CMakeLists.txt)
    add_subdirectory(libs/googletest)
endif()
#### AUTO-Generated: libs end ####

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)

