# hnxpack_core: the packaging pipeline, shared by the CLI and the GUI.
#
# Split out of the CLI so the GUI can call buildPackage() directly instead of
# shelling out to hnxpack.exe. That matters for more than tidiness: in-process
# means real progress callbacks, real cancellation, and structured Error values
# rather than scraped stderr text.
#
# Unlike the stub, this is build-time code, so <print>/<format>/<filesystem> are
# used freely and binary size is irrelevant.

add_library(hnxpack_core STATIC
    build.cpp
    walk.cpp
    blockize.cpp
    compress.cpp
    patch_rsrc.cpp
    icons.cpp
    appid.cpp
    cmdline.cpp
    embedded_stub.cpp
    console.cpp
    dialogs.cpp

    # Shared with the stub rather than duplicated. The packer must parse exactly
    # what the stub parses -- if these two ever drifted, hnxpack verify would be
    # validating a format nothing actually reads.
    ${CMAKE_SOURCE_DIR}/src/stub/toc.cpp
    ${CMAKE_SOURCE_DIR}/src/stub/selfmap.cpp
    ${CMAKE_SOURCE_DIR}/src/stub/meta.cpp
    ${CMAKE_SOURCE_DIR}/src/stub/codec/codec.cpp
)

target_include_directories(hnxpack_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/stub)

target_link_libraries(hnxpack_core
    PUBLIC  hnx_common zstd_full lz4 xxhash
    PRIVATE hnx_warnings)

# bcrypt: BCryptGenRandom for the build id, which must not collide across builds.
# version: reading VERSIONINFO out of the chosen entry point.
# ole32/shell32: the IFileDialog pickers.
target_link_libraries(hnxpack_core PUBLIC
    kernel32 user32 advapi32 bcrypt version ole32 shell32)

# ---------------------------------------------------------------------------
# The embedded stubs.
#
# Carried as resources so the packer is self-sufficient: the original failure
# this fixes was defaulting to a hnxstub.exe sitting beside the executable, which
# is not there once the packer is copied anywhere else.
#
# Exposed as a variable rather than compiled into the core library, because a
# resource has to be linked into each final executable that wants it -- both the
# CLI and the GUI read these from their own module.
# ---------------------------------------------------------------------------
set(HNX_STUBS_RC "HNXSTUB_X64 RCDATA \"${HNX_STUB_DIR}/hnxstub_x64.exe\"\n")
set(HNX_STUB_DEPS "${HNX_STUB_DIR}/hnxstub_x64.exe")

if(HNX_BUILD_X86_STUB)
    string(APPEND HNX_STUBS_RC "HNXSTUB_X86 RCDATA \"${HNX_STUB_DIR}/hnxstub_x86.exe\"\n")
    list(APPEND HNX_STUB_DEPS "${HNX_STUB_DIR}/hnxstub_x86.exe")
endif()

file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/stubs.rc CONTENT "${HNX_STUBS_RC}")

# rc does not track the binaries it embeds, so without this a rebuilt stub would
# not make it into the packer.
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/stubs.rc PROPERTIES
    OBJECT_DEPENDS "${HNX_STUB_DEPS}")

set(HNX_STUBS_RC_FILE ${CMAKE_CURRENT_BINARY_DIR}/stubs.rc CACHE INTERNAL "embedded stubs")

# Anything linking the stubs also needs them built first.
function(hnx_use_embedded_stubs target)
    add_dependencies(${target} hnxstub_x64_collected)
    if(HNX_BUILD_X86_STUB)
        add_dependencies(${target} hnxstub_x86)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# hnxpack: the command line tool
# ---------------------------------------------------------------------------

add_executable(hnxpack
    main.cpp
    interactive.cpp
    packer.rc
    ${HNX_STUBS_RC_FILE}
)

target_link_libraries(hnxpack PRIVATE hnxpack_core hnx_warnings)

if(MSVC)
    # The manifest comes from packer.rc, so the linker must not generate a second
    # one: two manifests in one binary is a load-time failure.
    target_link_options(hnxpack PRIVATE /MANIFEST:NO)
endif()

hnx_use_embedded_stubs(hnxpack)
