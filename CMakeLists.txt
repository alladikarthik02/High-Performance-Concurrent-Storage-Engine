cmake_minimum_required(VERSION 3.22)
project(lsmeng CXX)

# SPEC 1.1: zero third-party libraries. If a find_package() ever appears in this file,
# the project's central claim has been broken. The only things linked are libstdc++ and
# pthreads.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Frame pointers are kept in every configuration on purpose: SPEC 10.4 expects to fall
# back to `perf record -e cpu-clock`, whose call graphs are unusable without them.
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
                    -Wno-unused-parameter -fno-omit-frame-pointer -g)

# ---------------------------------------------------------------------------
# Sanitizer configuration. SPEC S20 / scripts/check.sh.
# ASan and TSan cannot coexist in one binary; hence three build trees.
# ---------------------------------------------------------------------------
set(LSMENG_SANITIZE "none" CACHE STRING "none | address | thread")
set_property(CACHE LSMENG_SANITIZE PROPERTY STRINGS none address thread)

if(LSMENG_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -O1)
  add_link_options(-fsanitize=address,undefined)
elseif(LSMENG_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -O1)
  add_link_options(-fsanitize=thread)
elseif(LSMENG_SANITIZE STREQUAL "none")
  add_compile_options(-O2)
else()
  message(FATAL_ERROR "LSMENG_SANITIZE must be none|address|thread, got '${LSMENG_SANITIZE}'")
endif()

# SPEC S12/S13/S23 are enforced by assertions that must be live in the two debug-ish
# configurations. A build with NDEBUG set would silently disable the only mechanism that
# detects a lock held across I/O, so we assert their absence rather than hope.
if(NOT LSMENG_SANITIZE STREQUAL "none")
  add_compile_definitions(LSMENG_ASSERTIONS=1)
endif()
add_compile_definitions(LSMENG_ASSERTIONS=1)   # also on in `none`: cost is one branch

include_directories(${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR})

# ---------------------------------------------------------------------------
# The engine library. Sources are listed explicitly, never globbed: a glob makes a
# forgotten file silently vanish from the build and reappear as a link error days later.
# ---------------------------------------------------------------------------
add_library(lsmeng STATIC
  src/env_posix.cc
  src/crc32c.cc
  src/stats.cc
  src/memtable.cc
  src/write_batch.cc
  src/wal.cc
  src/block.cc
  src/sst.cc
  src/cache.cc
  src/table_cache.cc
  src/version_edit.cc
  src/version_set.cc
)
target_link_libraries(lsmeng PUBLIC pthread)

# ---------------------------------------------------------------------------
# Tests. Each is a standalone binary using tests/test.h (no gtest -- SPEC 1.1).
# ---------------------------------------------------------------------------
enable_testing()

function(lsmeng_test name)
  add_executable(${name} tests/${name}.cc)
  target_link_libraries(${name} PRIVATE lsmeng)
  add_test(NAME ${name} COMMAND ${name})
  # Sanitizer runtimes need to abort loudly rather than print and continue.
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=abort_on_error=1:detect_leaks=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1;TSAN_OPTIONS=halt_on_error=1")
endfunction()

lsmeng_test(test_harness)
lsmeng_test(test_env)
lsmeng_test(test_coding)
lsmeng_test(test_crc32c)
lsmeng_test(test_dbformat)
lsmeng_test(test_arena)
lsmeng_test(test_hash)
lsmeng_test(test_stats)
lsmeng_test(test_bloom)
lsmeng_test(test_skiplist)
lsmeng_test(test_memtable)
lsmeng_test(test_wal)
lsmeng_test(test_block)
lsmeng_test(test_sst)
lsmeng_test(test_cache)
lsmeng_test(test_table_cache)
lsmeng_test(test_version_set)
lsmeng_test(test_lock_exclusion)

# ---------------------------------------------------------------------------
# Tools. SPEC 4.3.
# ---------------------------------------------------------------------------
add_executable(env_facts tools/env_facts.cc)
target_link_libraries(env_facts PRIVATE lsmeng)

add_executable(sst_dump tools/sst_dump.cc)
target_link_libraries(sst_dump PRIVATE lsmeng)
