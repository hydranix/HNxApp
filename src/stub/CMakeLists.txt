# The launcher stub. Every byte here ships inside every packaged application, so
# this target is deliberately frugal in a way the packer is not.
#
# Measured on this toolchain (MSVC 19.51, /MT /O2 /GL, GUI subsystem):
#   minimal binary using <expected>, <vector>, <string>   ~101 KB
#   the same binary after adding one std::println call    ~391 KB
#
# That ~290 KB delta is why nothing under src/stub/ may include <print>, <format>,
# <iostream> or <sstream>. User-facing text goes through Win32 directly
# (wsprintfW, MessageBoxW, WriteConsoleW). The packer has no such restriction.

add_executable(hnxstub WIN32
    main.cpp
    selfmap.cpp
    meta.cpp
    toc.cpp
    verify.cpp
    extract.cpp
    state.cpp
    launch.cpp
    firewall.cpp
    ui.cpp
    winpe.cpp
    codec/codec.cpp
    stub.rc
)

target_link_libraries(hnxstub PRIVATE
    hnx_common
    zstd_dec
    lz4
    xxhash
)

# comctl32 is deliberately absent: ui.cpp loads it with LoadLibraryW so that an
# image without it -- WinPE -- loses the progress bar rather than the process.
# fwpuclnt is absent for the same reason: firewall.cpp loads it at runtime, so an
# image without it loses firewall rules rather than failing to start at all.
target_link_libraries(hnxstub PRIVATE
    kernel32 user32 shell32 advapi32 shlwapi
)

target_include_directories(hnxstub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(hnxstub PROPERTIES
    OUTPUT_NAME "hnxstub"
    VS_DPI_AWARE "PerMonitor")

if(MSVC)
    # The manifest is embedded through stub.rc instead of being generated, so the
    # linker must not add a second one.
    target_link_options(hnxstub PRIVATE /MANIFEST:NO)
endif()

# Guard the size rule mechanically rather than by convention: a stray include
# would otherwise cost ~290 KB in every packaged app and nobody would notice.
add_custom_command(TARGET hnxstub POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DSTUB_EXE=$<TARGET_FILE:hnxstub>
            -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckStubSize.cmake
    VERBATIM)

# Collect this build's stub under an architecture-qualified name, ready for the
# packer to embed.
#
# Declared as a real OUTPUT rather than a POST_BUILD side effect: the packer's
# resource script names this file as an input, and Ninja refuses to build
# something whose dependency has no rule producing it.
#
# Guarded on HNX_STUB_ONLY: inside the 32-bit sub-build this same file runs
# again, and without the guard it would copy the x86 stub over hnxstub_x64.exe
# and the packer would ship two 32-bit stubs.
if(NOT HNX_STUB_ONLY)
    add_custom_command(
        OUTPUT ${HNX_STUB_DIR}/hnxstub_x64.exe
        COMMAND ${CMAKE_COMMAND} -E make_directory ${HNX_STUB_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:hnxstub> ${HNX_STUB_DIR}/hnxstub_x64.exe
        DEPENDS hnxstub
        COMMENT "Collecting x64 stub for embedding"
        VERBATIM)

    add_custom_target(hnxstub_x64_collected
        DEPENDS ${HNX_STUB_DIR}/hnxstub_x64.exe)
endif()
