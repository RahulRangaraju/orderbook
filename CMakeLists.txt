cmake_minimum_required(VERSION 3.20)
project(orderbook CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

# Step 8: -DOB_SANITIZE=thread builds everything under ThreadSanitizer
#         (also accepts address / undefined).
set(OB_SANITIZE "" CACHE STRING "Sanitizer to enable: thread, address, undefined")
if(OB_SANITIZE)
  add_compile_options(-fsanitize=${OB_SANITIZE} -g -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${OB_SANITIZE})
endif()

# Step 9: -DOB_PROFILE=ON keeps symbols and frame pointers in the optimized
#         build so perf can show call stacks.
option(OB_PROFILE "Release build with symbols for perf" OFF)
if(OB_PROFILE)
  add_compile_options(-g -fno-omit-frame-pointer)
endif()

find_package(Threads REQUIRED)

add_library(ob src/order_book.cpp src/flat_order_book.cpp)
target_include_directories(ob PUBLIC include)
target_link_libraries(ob PUBLIC Threads::Threads)

add_executable(replay apps/replay.cpp)
target_link_libraries(replay ob)

add_executable(gen apps/gen.cpp)
target_link_libraries(gen ob)

# Record the flags in the binary so benchmark output says how it was built.
string(TOUPPER "${CMAKE_BUILD_TYPE}" OB_BT)
add_executable(bench apps/bench.cpp)
target_link_libraries(bench ob)
target_compile_definitions(bench PRIVATE
  OB_BUILD_FLAGS="${CMAKE_BUILD_TYPE} ${CMAKE_CXX_FLAGS_${OB_BT}}")

include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(googletest)
enable_testing()
add_executable(tests
  tests/test_order_book.cpp
  tests/test_randomized.cpp
  tests/test_parser.cpp
  tests/test_spsc.cpp)
target_link_libraries(tests ob GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 120)
