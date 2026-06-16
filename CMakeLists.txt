cmake_minimum_required(VERSION 3.16)
project(tracy-mcp LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Tracy's Worker uses lzcnt/popcnt/bmi intrinsics; without ISA flags __lzcnt64
# fails to inline (GCC/Clang).
#  - local default: -march=native (fastest, this machine only)
#  - PORTABLE: x86-64-v3 baseline (AVX2/BMI/LZCNT, ~2014+ CPUs) for distributed
#    binaries. -march=native would bake in the BUILD machine's ISA and SIGILL on
#    older user CPUs -- always set PORTABLE=ON in release CI.
option(TRACY_MCP_PORTABLE "Use a portable ISA baseline for distributable binaries" OFF)
if(MSVC)
    add_compile_options(/arch:AVX2)
else()
    include(CheckCXXCompilerFlag)
    if(TRACY_MCP_PORTABLE)
        # x86-64-v3 is x86-only; on arm64 (e.g. macos-latest) clang rejects it and
        # the x86 intrinsics aren't compiled anyway, so skip when unsupported.
        check_cxx_compiler_flag("-march=x86-64-v3" HAS_MARCH_V3)
        if(HAS_MARCH_V3)
            add_compile_options(-march=x86-64-v3)
        endif()
    else()
        check_cxx_compiler_flag("-march=native" HAS_MARCH_NATIVE)
        if(HAS_MARCH_NATIVE)
            add_compile_options(-march=native)
        endif()
    endif()
endif()
if(WIN32)
    add_definitions(-DNOMINMAX -DWIN32_LEAN_AND_MEAN)
endif()

set(TRACY_DIR ${CMAKE_CURRENT_LIST_DIR}/third_party/tracy)

# Deps (capstone, zstd, ppqsort, json) -- must come before server.cmake,
# which links TracyCapstone / libzstd / PPQSort::PPQSort by name.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/deps.cmake)

# Builds the TracyServer static lib from Tracy's own recipe (Worker + friends).
include(${TRACY_DIR}/cmake/server.cmake)

add_executable(tracy-mcp
    src/main.cpp
    src/tools.cpp
)
target_link_libraries(tracy-mcp PRIVATE TracyServer nlohmann_json::nlohmann_json)

if(WIN32)
    target_link_libraries(tracy-mcp PRIVATE ws2_32)
endif()
if(MINGW)
    # Static-link the MinGW runtime so the binary runs without msys64 on PATH
    # (MCP clients launch it with a bare environment). MSVC uses the system CRT.
    target_link_options(tracy-mcp PRIVATE -static -static-libgcc -static-libstdc++)
endif()

include(GNUInstallDirs)
install(TARGETS tracy-mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
