# Minimal dependency set for the Tracy *server* library only.
# Tracy's own cmake/vendor.cmake pulls the whole GUI stack (glfw, imgui,
# freetype, curl, tidy, usearch, ...). The server/Worker lib needs just three:
# capstone, zstd, ppqsort. We add nlohmann/json for our own MCP layer.
# Target names (TracyCapstone / libzstd / PPQSort::PPQSort) must match what
# third_party/tracy/cmake/server.cmake links against.

include(${CMAKE_CURRENT_LIST_DIR}/CPM.cmake)

set(TRACY_DIR ${CMAKE_CURRENT_LIST_DIR}/../third_party/tracy)

# capstone -- Worker uses it for symbol/instruction decoding
CPMAddPackage(
    NAME capstone
    GITHUB_REPOSITORY capstone-engine/capstone
    GIT_TAG 6.0.0-Alpha1
    OPTIONS
        "CAPSTONE_X86_ATT_DISABLE ON"
        "CAPSTONE_ALPHA_SUPPORT OFF"
        "CAPSTONE_HPPA_SUPPORT OFF"
        "CAPSTONE_LOONGARCH_SUPPORT OFF"
        "CAPSTONE_M680X_SUPPORT OFF"
        "CAPSTONE_M68K_SUPPORT OFF"
        "CAPSTONE_MIPS_SUPPORT OFF"
        "CAPSTONE_MOS65XX_SUPPORT OFF"
        "CAPSTONE_PPC_SUPPORT OFF"
        "CAPSTONE_SPARC_SUPPORT OFF"
        "CAPSTONE_SYSTEMZ_SUPPORT OFF"
        "CAPSTONE_XCORE_SUPPORT OFF"
        "CAPSTONE_TRICORE_SUPPORT OFF"
        "CAPSTONE_TMS320C64X_SUPPORT OFF"
        "CAPSTONE_EVM_SUPPORT OFF"
        "CAPSTONE_WASM_SUPPORT OFF"
        "CAPSTONE_BPF_SUPPORT OFF"
        "CAPSTONE_RISCV_SUPPORT OFF"
        "CAPSTONE_SH_SUPPORT OFF"
        "CAPSTONE_XTENSA_SUPPORT OFF"
        "CAPSTONE_BUILD_MACOS_THIN ON"
    EXCLUDE_FROM_ALL TRUE
)
add_library(TracyCapstone INTERFACE)
target_include_directories(TracyCapstone INTERFACE ${capstone_SOURCE_DIR}/include/capstone)
target_link_libraries(TracyCapstone INTERFACE capstone)

# zstd -- .tracy compression. Exposes the `libzstd` target server.cmake links.
CPMAddPackage(
    NAME zstd
    GITHUB_REPOSITORY facebook/zstd
    GIT_TAG v1.5.7
    OPTIONS
        "ZSTD_BUILD_SHARED OFF"
    EXCLUDE_FROM_ALL TRUE
    SOURCE_SUBDIR build/cmake
)

# ppqsort -- parallel sort used while building zone statistics.
# Tracy's ppqsort-nodebug.patch only silences a Debug-build assert; we build
# Release, and re-applying a patch on reconfigure isn't idempotent, so skip it.
CPMAddPackage(
    NAME PPQSort
    GITHUB_REPOSITORY GabTux/PPQSort
    VERSION 1.0.5
    EXCLUDE_FROM_ALL TRUE
)

# nlohmann/json -- our MCP/JSON-RPC layer (also a Tracy dep, same version)
CPMAddPackage(
    NAME json
    GITHUB_REPOSITORY nlohmann/json
    GIT_TAG v3.12.0
    EXCLUDE_FROM_ALL TRUE
)
