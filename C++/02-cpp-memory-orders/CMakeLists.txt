cmake_minimum_required(VERSION 3.5)
project(mo_memory_orders LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(mo_demo src/main.cpp)
target_include_directories(mo_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mo_demo PRIVATE Threads::Threads)

add_executable(mo_tests
    tests/test_main.cpp
    tests/test_spsc.cpp
    tests/test_config.cpp
    tests/test_ref_counted.cpp
    tests/test_lifecycle.cpp
    tests/test_seq_cst.cpp
    tests/test_metrics.cpp
)
target_include_directories(mo_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mo_tests PRIVATE Threads::Threads)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mo_demo PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    target_compile_options(mo_tests PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

enable_testing()
add_test(NAME memory_order_tests COMMAND mo_tests)
