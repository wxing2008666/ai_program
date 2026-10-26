cmake_minimum_required(VERSION 3.10)
project(vtable_polymorphism_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(DEMO_WARNINGS "")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(DEMO_WARNINGS -Wall -Wextra -Wpedantic -Wnon-virtual-dtor)
endif()

add_library(risk_core STATIC src/risk.cpp)
target_include_directories(risk_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(risk_core PRIVATE ${DEMO_WARNINGS})

add_executable(vtable_demo src/main.cpp src/abi_probe.cpp)
target_link_libraries(vtable_demo PRIVATE risk_core)
target_compile_options(vtable_demo PRIVATE ${DEMO_WARNINGS})

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # ABI 探针会按 Itanium 布局读取 vptr/vtable；避免优化器按严格别名规则重排读取。
    target_compile_options(vtable_demo PRIVATE -fno-strict-aliasing)
endif()

option(ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(ENABLE_SANITIZERS)
    target_compile_options(risk_core PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_compile_options(vtable_demo PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_libraries(vtable_demo PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()
add_test(NAME vtable_demo COMMAND vtable_demo)
