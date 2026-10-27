cmake_minimum_required(VERSION 3.25)
project(engineering_interview_roadmap LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Toolchain: C++23, no compiler extensions.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Optional sanitizers, e.g. -DSANITIZE=thread or -DSANITIZE=address,undefined
set(SANITIZE "" CACHE STRING "Comma-separated sanitizers passed to -fsanitize= (empty = off)")

find_package(Threads REQUIRED)
enable_testing()

# Shared compile settings for every target.
function(configure_target target)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/common/cpp)
    target_link_libraries(${target} PRIVATE Threads::Threads)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    endif()
    if(SANITIZE)
        target_compile_options(${target} PRIVATE -fsanitize=${SANITIZE} -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=${SANITIZE})
    endif()
endfunction()

# add_exercise(<name>)
#   Builds <name>_demo from main.cpp and <name>_test from test.cpp in the calling
#   directory, and registers <name>_test with CTest.
function(add_exercise name)
    add_executable(${name}_demo main.cpp)
    add_executable(${name}_test test.cpp)
    configure_target(${name}_demo)
    configure_target(${name}_test)
    add_test(NAME ${name} COMMAND ${name}_test)
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

# add_program(<name>)
#   Builds a single executable <name> from main.cpp (implementation + demo, no unit tests).
function(add_program name)
    add_executable(${name} main.cpp)
    configure_target(${name})
endfunction()

# ---------------------------------------------------------------------------
# Exercises
# ---------------------------------------------------------------------------
add_subdirectory(05-system-programming/concurrency-synchronization/bounded-queue)
add_subdirectory(01-coding-fundamentals/data-structures/hash-maps/lru-cache)
add_subdirectory(06-networking/sockets/concurrent-tcp-server)
add_subdirectory(05-system-programming/concurrency-synchronization/data-race-fix)
add_subdirectory(04-system-design/scalability-performance/rate-limiter)
add_subdirectory(05-system-programming/processes-threads/worker-pool)
add_subdirectory(06-networking/tcp-ip/stream-message-framing)
add_subdirectory(05-system-programming/concurrency-synchronization/producer-consumer-backpressure)
add_subdirectory(05-system-programming/debugging-profiling/deadlock-diagnosis)
add_subdirectory(04-system-design/databases-caching/expiring-cache)
