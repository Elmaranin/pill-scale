# Host tests of the algorithms (src/algo/), no board needed.
#
#   cmake -S test -B test/build && cmake --build test/build && ctest --test-dir test/build
#
# The Unity framework comes from ESP-IDF, so an activated IDF ($IDF_PATH) is required.
# The compiler is the ordinary host one, not the ESP32 toolchain.

cmake_minimum_required(VERSION 3.22)
project(pillscale_host_tests C CXX)

set(CMAKE_CXX_STANDARD 17)

if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR "IDF_PATH is not set: activate ESP-IDF first (see README.md)")
endif()

set(UNITY_SRC $ENV{IDF_PATH}/components/unity/unity/src)
add_library(unity STATIC ${UNITY_SRC}/unity.c)
target_include_directories(unity PUBLIC ${UNITY_SRC})
# without it TEST_ASSERT_*_DOUBLE fail with "Unity Double Precision Disabled"
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)

enable_testing()

# Every test is a folder test_<name>/test_<name>.cpp
file(GLOB test_dirs RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/test_*)
foreach(t ${test_dirs})
    add_executable(${t} ${t}/${t}.cpp)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/algo)
    target_link_libraries(${t} PRIVATE unity)
    # scale recordings for tests on real data
    target_compile_definitions(${t} PRIVATE PILLSCALE_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../docs/data")
    add_test(NAME ${t} COMMAND ${t})
endforeach()
