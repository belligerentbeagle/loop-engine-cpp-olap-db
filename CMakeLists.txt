cmake_minimum_required(VERSION 3.20)
project(strata LANGUAGES CXX
        DESCRIPTION "A vectorized, columnar OLAP engine for advertising event logs")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
export(PACKAGE strata)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------------
# "Fast" flags: -O3 plus the best available native-tuning flag. On x86 -march=native
# unlocks AVX; on Apple arm64 NEON is baseline and -mcpu=native tunes for the chip.
# We probe what the compiler accepts and fall back gracefully.
# ---------------------------------------------------------------------------------
include(CheckCXXCompilerFlag)
add_library(strata_flags INTERFACE)
target_compile_options(strata_flags INTERFACE -O3 -Wall -Wextra)
check_cxx_compiler_flag("-march=native" HAS_MARCH_NATIVE)
if(HAS_MARCH_NATIVE)
    target_compile_options(strata_flags INTERFACE -march=native)
else()
    check_cxx_compiler_flag("-mcpu=native" HAS_MCPU_NATIVE)
    if(HAS_MCPU_NATIVE)
        target_compile_options(strata_flags INTERFACE -mcpu=native)
    endif()
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------------
# Core engine (static, position-independent so it can be linked into the Python module).
# ---------------------------------------------------------------------------------
add_library(strata_core STATIC
    src/parser.cpp
    src/table.cpp
    src/executor.cpp
    src/threadpool.cpp
    src/memtable.cpp)
target_include_directories(strata_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(strata_core PUBLIC strata_flags Threads::Threads)
set_target_properties(strata_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- CLI ------------------------------------------------------------------------
add_executable(strata_cli apps/strata_cli.cpp)
target_link_libraries(strata_cli PRIVATE strata_core)
set_target_properties(strata_cli PROPERTIES OUTPUT_NAME strata)

# ---- tests ----------------------------------------------------------------------
enable_testing()
add_executable(strata_tests tests/test_strata.cpp)
target_link_libraries(strata_tests PRIVATE strata_core)
add_test(NAME strata_tests COMMAND strata_tests)

# ---- benchmarks: O3 -march=native, plus an -O2 twin for the codegen comparison ---
add_executable(strata_bench bench/bench_scan.cpp)
target_link_libraries(strata_bench PRIVATE strata_core)

add_executable(strata_bench_o2 bench/bench_scan.cpp)
target_include_directories(strata_bench_o2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(strata_bench_o2 PRIVATE strata_core)
# Override to a plain -O2 build of the benchmark TU (kernels live in the TU, so this is the
# apples-to-apples O2 baseline vs the O3 -march=native target above).
target_compile_options(strata_bench_o2 PRIVATE -O2)

# -O0 twin: scalar, un-vectorized. The headline kernel here vs the O3 build is the real
# "scalar code -> SIMD" win (on Apple Silicon NEON is baseline, so O2≈O3 but O0≪both).
add_executable(strata_bench_o0 bench/bench_scan.cpp)
target_include_directories(strata_bench_o0 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(strata_bench_o0 PRIVATE strata_core)
target_compile_options(strata_bench_o0 PRIVATE -O0)

# ---------------------------------------------------------------------------------
# Python bindings (optional). Found via `python -m pybind11 --cmakedir`.
# Build with: cmake -DSTRATA_PYTHON=ON ...   (auto-enabled if pybind11 is discoverable)
# ---------------------------------------------------------------------------------
option(STRATA_PYTHON "Build the pybind11 Python module" ON)
if(STRATA_PYTHON)
    # Locate the interpreter explicitly (CMake's default pick on macOS can be a framework
    # Python without dev libs). Honor a user-supplied -DPython3_EXECUTABLE if given.
    if(NOT Python3_EXECUTABLE)
        find_program(Python3_EXECUTABLE NAMES python3 python)
    endif()
    if(Python3_EXECUTABLE)
        set(PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")  # hint for pybind11's own Python find
        find_package(Python3 COMPONENTS Interpreter Development.Module QUIET)
        execute_process(COMMAND ${Python3_EXECUTABLE} -m pybind11 --cmakedir
                        OUTPUT_VARIABLE PYBIND11_CMAKE_DIR
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        RESULT_VARIABLE PYBIND11_RC ERROR_QUIET)
        if(PYBIND11_RC EQUAL 0)
            list(APPEND CMAKE_PREFIX_PATH "${PYBIND11_CMAKE_DIR}")
            find_package(pybind11 CONFIG QUIET)
        endif()
    endif()
    if(pybind11_FOUND)
        pybind11_add_module(strata bindings/strata_py.cpp)
        target_link_libraries(strata PRIVATE strata_core)
        message(STATUS "strata: Python module ENABLED (pybind11 ${pybind11_VERSION})")
    else()
        message(STATUS "strata: Python module SKIPPED (pybind11 not found; pip install pybind11)")
    endif()
endif()

message(STATUS "strata: build type ${CMAKE_BUILD_TYPE}, native flags probed (march=${HAS_MARCH_NATIVE})")
