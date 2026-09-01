cmake_minimum_required(VERSION 3.25)
project(HNxApp VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "HNx targets Windows only.")
endif()

set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC exposes no /std:c++26 flag; the C++26 track is /std:c++latest. Everything
# this project relies on (std::expected, std::print, std::jthread) is reachable
# there. The flag is set directly rather than through CMAKE_CXX_STANDARD so the
# two cannot disagree about which mode we actually get.
#
# The C++ options are scoped to COMPILE_LANGUAGE:CXX because the vendored zstd
# and lz4 are C, and passing /std:c++latest to a C file is a command-line warning
# on every single translation unit.
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/std:c++latest>
                        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
                        $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
                        $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
    # /W4 is deliberately NOT global: it would apply to the vendored C sources
    # too, and silencing it there with /W0 produces a D9025 override warning on
    # every translation unit. Our own targets opt in via hnx_warnings.
    add_compile_options(/utf-8 /Zc:preprocessor)
    add_compile_options(/guard:cf /GS)
    add_compile_options($<$<CONFIG:Release>:/O2> $<$<CONFIG:Release>:/Gy> $<$<CONFIG:Release>:/GL>)
    add_link_options($<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF> $<$<CONFIG:Release>:/LTCG>)
    add_link_options(/DYNAMICBASE /NXCOMPAT)

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        # 64-bit only; the linker rejects it outright on an x86 target.
        add_link_options(/HIGHENTROPYVA)
    else()
        # The stub memory-maps the entire payload, so on 32-bit the address space
        # is the binding limit on package size. This lifts the usable range from
        # 2 GB to 3 GB (4 GB when the image runs under WOW64), which is the
        # difference between a 1.5 GB package working and not.
        add_link_options(/LARGEADDRESSAWARE)
    endif()
    # Static CRT so a packaged stub has zero runtime dependencies: it may run on a
    # machine that has never seen a VC++ redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
                            _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Build modes
#
# The packer embeds a stub for each target architecture as a resource, so a
# packaged application can be produced from hnxpack alone with nothing else on
# disk beside it. Building an x86 stub from an x64 build needs a second CMake
# configure against a different toolset, which is what HNX_STUB_ONLY guards: the
# sub-build re-enters this same CMakeLists with that flag set and defines only
# the stub, so it cannot recurse into building itself again.
# ---------------------------------------------------------------------------

option(HNX_STUB_ONLY "Build only the stub. Set by the cross-architecture sub-build." OFF)
option(HNX_BUILD_X86_STUB "Also build a 32-bit stub and embed it in hnxpack." ON)

# Where the collected per-architecture stubs land, ready to be embedded.
set(HNX_STUB_DIR ${CMAKE_BINARY_DIR}/stubs CACHE INTERNAL "collected stub binaries")

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------------------
# Third-party sources.
#
# SOURCE_SUBDIR points at a directory with no CMakeLists.txt on purpose: that
# makes FetchContent download and populate without configuring the upstream
# build. We compile the sources ourselves so the stub can link a decompress-only
# zstd while the packer links the full library -- two variants that upstream's
# single CMake target cannot give us.
# ---------------------------------------------------------------------------

FetchContent_Declare(zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.6
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  lib)

FetchContent_Declare(lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG        v1.10.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  lib)

FetchContent_Declare(xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.3
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  cmake_unofficial_disabled)

# Dear ImGui for the GUI.
#
# Chosen because it keeps the project's defining property: one self-contained
# executable with nothing to install. Its DX11 backend uses a library that ships
# with Windows, so the GUI has no runtime dependency at all -- unlike Qt (DLLs or
# LGPL static linking), WinUI (the App SDK runtime) or WebView2 (a runtime that is
# usually but not always present).
#
# SOURCE_SUBDIR points at a directory with no CMakeLists.txt for the same reason
# as the codecs: we compile the handful of sources ourselves rather than adopt an
# upstream build.
if(NOT HNX_STUB_ONLY)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.8
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  misc)
    FetchContent_MakeAvailable(zstd lz4 xxhash imgui)
else()
    FetchContent_MakeAvailable(zstd lz4 xxhash)
endif()

# --- zstd common sources, shared by both variants ---------------------------
set(ZSTD_COMMON_SRC
    ${zstd_SOURCE_DIR}/lib/common/debug.c
    ${zstd_SOURCE_DIR}/lib/common/entropy_common.c
    ${zstd_SOURCE_DIR}/lib/common/error_private.c
    ${zstd_SOURCE_DIR}/lib/common/fse_decompress.c
    ${zstd_SOURCE_DIR}/lib/common/pool.c
    ${zstd_SOURCE_DIR}/lib/common/threading.c
    ${zstd_SOURCE_DIR}/lib/common/xxhash.c
    ${zstd_SOURCE_DIR}/lib/common/zstd_common.c)

set(ZSTD_DECOMPRESS_SRC
    ${zstd_SOURCE_DIR}/lib/decompress/huf_decompress.c
    ${zstd_SOURCE_DIR}/lib/decompress/zstd_ddict.c
    ${zstd_SOURCE_DIR}/lib/decompress/zstd_decompress.c
    ${zstd_SOURCE_DIR}/lib/decompress/zstd_decompress_block.c)

set(ZSTD_COMPRESS_SRC
    ${zstd_SOURCE_DIR}/lib/compress/fse_compress.c
    ${zstd_SOURCE_DIR}/lib/compress/hist.c
    ${zstd_SOURCE_DIR}/lib/compress/huf_compress.c
    ${zstd_SOURCE_DIR}/lib/compress/zstd_compress.c
    ${zstd_SOURCE_DIR}/lib/compress/zstd_compress_literals.c
    ${zstd_SOURCE_DIR}/lib/compress/zstd_compress_sequences.c
    ${zstd_SOURCE_DIR}/lib/compress/zstd_compress_superblock.c
    ${zstd_SOURCE_DIR}/lib/compress/zstd_double_fast.c
    ${zstd_SOURCE_DIR}/lib/compress/zstd_fast.c
    ${zstd_SOURCE_DIR}/lib/compress/zstd_lazy.c
    ${zstd_SOURCE_DIR}/lib/compress/zstd_ldm.c
    ${zstd_SOURCE_DIR}/lib/compress/zstd_opt.c
    ${zstd_SOURCE_DIR}/lib/compress/zstd_preSplit.c
    ${zstd_SOURCE_DIR}/lib/compress/zstdmt_compress.c)
# zstd_preSplit.c only exists from 1.5.7 onward; drop it silently when absent so
# bumping the pinned tag in either direction does not break the build.
foreach(_f ${ZSTD_COMPRESS_SRC})
    if(NOT EXISTS ${_f})
        list(REMOVE_ITEM ZSTD_COMPRESS_SRC ${_f})
    endif()
endforeach()

# Decompress-only variant for the stub. This is the difference between roughly
# 80 KB and roughly 500 KB of zstd in the shipped binary.
add_library(zstd_dec STATIC ${ZSTD_COMMON_SRC} ${ZSTD_DECOMPRESS_SRC})
target_include_directories(zstd_dec PUBLIC ${zstd_SOURCE_DIR}/lib)
target_compile_definitions(zstd_dec PRIVATE
    ZSTD_LIB_COMPRESSION=0 ZSTD_LIB_DICTBUILDER=0 ZSTD_LIB_DEPRECATED=0
    ZSTD_LEGACY_SUPPORT=0 ZSTD_MULTITHREAD=0
    ZSTD_DISABLE_ASM=1)   # the .S fast path is unusable under MSVC

# Full variant for the packer.
add_library(zstd_full STATIC ${ZSTD_COMMON_SRC} ${ZSTD_DECOMPRESS_SRC} ${ZSTD_COMPRESS_SRC})
target_include_directories(zstd_full PUBLIC ${zstd_SOURCE_DIR}/lib)
target_compile_definitions(zstd_full PRIVATE
    ZSTD_LEGACY_SUPPORT=0 ZSTD_MULTITHREAD=0 ZSTD_DISABLE_ASM=1)

# --- lz4 --------------------------------------------------------------------
# lz4.c holds both directions and is small enough that splitting it would save
# nothing worth the extra target.
add_library(lz4 STATIC ${lz4_SOURCE_DIR}/lib/lz4.c ${lz4_SOURCE_DIR}/lib/lz4hc.c)
target_include_directories(lz4 PUBLIC ${lz4_SOURCE_DIR}/lib)

# --- xxhash -----------------------------------------------------------------
#
# zstd and lz4 each vendor their own older xxhash.h, and both of their lib
# directories are on the include path. A bare #include <xxhash.h> therefore
# resolves by include order rather than intent, and lz4's copy predates XXH3 --
# which fails as a missing-identifier error rather than anything that points at
# the real cause.
#
# Copying the real headers under a uniquely named directory makes the include
# unambiguous no matter how the search order shakes out.
set(HNX_XXHASH_INC ${CMAKE_BINARY_DIR}/xxhash_include)
file(COPY ${xxhash_SOURCE_DIR}/xxhash.h ${xxhash_SOURCE_DIR}/xxh3.h
     DESTINATION ${HNX_XXHASH_INC}/hnx_xxhash)

add_library(xxhash INTERFACE)
target_include_directories(xxhash INTERFACE ${HNX_XXHASH_INC})
# XXH_INLINE_ALL already implies static linking, and adding
# XXH_STATIC_LINKING_ONLY on top collides with the vendored copies.
target_compile_definitions(xxhash INTERFACE XXH_INLINE_ALL)

# Warning level for our own code. Vendored C is left at the default rather than
# forced to /W0, which avoids the override warning entirely.
add_library(hnx_warnings INTERFACE)
if(MSVC)
    target_compile_options(hnx_warnings INTERFACE /W4)
endif()

# ---------------------------------------------------------------------------
# Project targets
# ---------------------------------------------------------------------------

add_library(hnx_common STATIC
    src/common/paths.cpp)
target_include_directories(hnx_common PUBLIC src/common)
target_link_libraries(hnx_common PUBLIC xxhash PRIVATE hnx_warnings)

add_subdirectory(src/stub)

# The sub-build stops here: it exists only to produce one stub binary.
if(NOT HNX_STUB_ONLY)

    # --- the 32-bit stub ----------------------------------------------------
    #
    # Built through a nested CMake configure with the Visual Studio generator,
    # which resolves the x86 toolset and SDK libraries itself. Doing this with
    # Ninja would mean reproducing the INCLUDE and LIB environment that VsDevCmd
    # sets up for a different target architecture, by hand.
    if(HNX_BUILD_X86_STUB)
        include(ExternalProject)

        ExternalProject_Add(hnxstub_x86
            SOURCE_DIR       ${CMAKE_SOURCE_DIR}
            BINARY_DIR       ${CMAKE_BINARY_DIR}/x86
            CMAKE_GENERATOR  "Visual Studio 18 2026"
            CMAKE_GENERATOR_PLATFORM Win32
            CMAKE_ARGS
                -DHNX_STUB_ONLY=ON
                -DCMAKE_BUILD_TYPE=Release
                -DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${CMAKE_BINARY_DIR}/x86out
                # Reuse the sources this build already fetched, so the sub-build
                # does not clone zstd, lz4 and xxHash a second time.
                -DFETCHCONTENT_SOURCE_DIR_ZSTD=${zstd_SOURCE_DIR}
                -DFETCHCONTENT_SOURCE_DIR_LZ4=${lz4_SOURCE_DIR}
                -DFETCHCONTENT_SOURCE_DIR_XXHASH=${xxhash_SOURCE_DIR}
            BUILD_COMMAND
                ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release --target hnxstub
            INSTALL_COMMAND
                ${CMAKE_COMMAND} -E copy_if_different
                    ${CMAKE_BINARY_DIR}/x86out/hnxstub.exe
                    ${HNX_STUB_DIR}/hnxstub_x86.exe
            # Declares the collected stub as a real output, so the resource script
            # that embeds it has a rule to depend on.
            BUILD_BYPRODUCTS ${HNX_STUB_DIR}/hnxstub_x86.exe
            # Without this, editing a stub source would rebuild the x64 stub and
            # silently leave a stale x86 one embedded in the packer.
            BUILD_ALWAYS ON
            USES_TERMINAL_BUILD ON)
    endif()

    # --- Dear ImGui ---------------------------------------------------------
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)

    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends)

    target_link_libraries(imgui PUBLIC d3d11 dxgi dwmapi)

    if(MSVC)
        target_compile_options(imgui PRIVATE /W0)  # upstream, not ours to lint
    endif()

    add_subdirectory(src/packer)
    add_subdirectory(src/gui)

    include(CTest)
    if(BUILD_TESTING)
        add_subdirectory(tests)
    endif()

    # -----------------------------------------------------------------------
    # Install
    #
    # Only the two user-facing programs. The stubs are already resources inside
    # both of them, so installing loose hnxstub binaries beside them would be
    # dead weight -- and would imply the packer looks for one on disk, which is
    # exactly the behaviour the embedded stubs replaced.
    #
    # bin/ rather than the CMake-on-Windows default of putting runtimes at the
    # prefix root, so the install prefix can be added to PATH without also
    # putting the prefix's other contents there.
    #
    # PDBs are installed alongside as OPTIONAL: a Release build with /Zi has
    # them, one without simply installs nothing rather than failing.
    # -----------------------------------------------------------------------
    install(TARGETS hnxpack HNxPacker
            RUNTIME DESTINATION bin)

    install(FILES $<TARGET_PDB_FILE:hnxpack> $<TARGET_PDB_FILE:HNxPacker>
            DESTINATION bin
            OPTIONAL)

endif()
