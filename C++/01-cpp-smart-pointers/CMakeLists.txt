cmake_minimum_required(VERSION 3.5)
project(spl_smart_pointers LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(spl_core STATIC
    src/allocation_probe.cpp
    src/codec.cpp
    src/frame.cpp
    src/frame_cache.cpp
    src/gpu_buffer.cpp
    src/object_graph.cpp
    src/pipeline.cpp
    src/plugin.cpp
    src/plugin_registry.cpp
    src/resource.cpp
    src/task_queue.cpp
)

target_include_directories(spl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(spl_core PUBLIC Threads::Threads)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(spl_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

add_executable(spl_demo src/main.cpp)
target_link_libraries(spl_demo PRIVATE spl_core)

add_executable(spl_tests
    tests/test_main.cpp
    tests/test_sizes.cpp
    tests/test_make_unique.cpp
    tests/test_intrusive.cpp
    tests/test_resources.cpp
    tests/test_frame.cpp
    tests/test_cache.cpp
    tests/test_graph.cpp
    tests/test_pipeline.cpp
    tests/test_async.cpp
    tests/test_registry.cpp
    tests/test_allocation.cpp
    tests/test_codec.cpp
    tests/test_casts.cpp
    tests/test_shared_slot_threads.cpp
)
target_link_libraries(spl_tests PRIVATE spl_core)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(spl_tests PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

enable_testing()
add_test(NAME smart_pointer_tests COMMAND spl_tests)
